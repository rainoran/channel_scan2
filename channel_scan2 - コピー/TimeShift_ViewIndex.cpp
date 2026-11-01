#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TimeShift_ViewIndex.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <fstream>
#include <system_error>
#include <unordered_map>
#include <utility>


namespace
{
    std::wstring SanitizeFileNamePart_(const std::wstring& s)
    {
        std::wstring out;
        out.reserve(s.size());

        for (const wchar_t ch : s) {
            switch (ch) {
            case L'<':
            case L'>':
            case L':':
            case L'"':
            case L'/':
            case L'\\':
            case L'|':
            case L'?':
            case L'*':
            case L'\r':
            case L'\n':
            case L'\t':
                out.push_back(L'_');
                break;

            default:
                if (ch < 0x20) {
                    out.push_back(L'_');
                }
                else {
                    out.push_back(ch);
                }
                break;
            }
        }

        if (out.empty()) {
            out = L"program";
        }

        if (out.size() > 160) {
            out.resize(160);
        }

        return out;
    }

    std::filesystem::path ResolveDefaultCacheRoot_(
        const TimeShift_ViewIndex::ProgramEntry& program,
        const std::filesystem::path& outputPath)
    {
        if (!program.joinSegments.empty()) {
            const auto segPath = program.joinSegments.front().filePath;

            // timeshift/space0_ch26/seg_xxx.ts
            //   parent_path()        -> space0_ch26
            //   parent_path().parent -> timeshift
            const auto channelFolder = segPath.parent_path();
            const auto rootFolder = channelFolder.parent_path();

            if (!rootFolder.empty()) {
                return rootFolder / L"cache" / L"join_work";
            }
        }

        const auto outParent = outputPath.parent_path();
        if (!outParent.empty()) {
            return outParent / L"cache" / L"join_work";
        }

        return L"timeshift\\cache\\join_work";
    }

    std::filesystem::path MakeWorkFolder_(
        const std::filesystem::path& cacheRoot,
        const std::wstring& programKey)
    {
        std::wstring name = SanitizeFileNamePart_(programKey);
        name += L"_";
        name += std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId()));
        name += L"_";
        name += std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));

        return cacheRoot / name;
    }

    bool LinkOrCopyFileToCache_(
        const std::filesystem::path& source,
        const std::filesystem::path& dest,
        bool useHardLink,
        std::wstring& reason) noexcept
    {
        reason.clear();

        try {
            std::error_code ec;

            if (!std::filesystem::exists(source, ec)) {
                reason = L"元セグメントが存在しません: ";
                reason += source.wstring();
                return false;
            }

            std::filesystem::create_directories(dest.parent_path(), ec);
            if (ec) {
                reason = L"キャッシュフォルダ作成に失敗しました: ";
                reason += dest.parent_path().wstring();
                return false;
            }

            std::filesystem::remove(dest, ec);

            if (useHardLink) {
                if (CreateHardLinkW(
                    dest.wstring().c_str(),
                    source.wstring().c_str(),
                    nullptr) != FALSE) {
                    return true;
                }
            }

            if (CopyFileW(
                source.wstring().c_str(),
                dest.wstring().c_str(),
                FALSE) == FALSE) {
                reason = L"セグメント退避に失敗しました: ";
                reason += source.wstring();
                return false;
            }

            return true;
        }
        catch (...) {
            reason = L"セグメント退避中に例外が発生しました。";
            return false;
        }
    }

    std::filesystem::path MakeNumberedCacheSegmentPath_(
        const std::filesystem::path& workFolder,
        std::size_t index)
    {
        wchar_t name[64]{};
        swprintf_s(
            name,
            L"%06zu.ts",
            index + 1);

        return workFolder / name;
    }

    bool RemoveFileNoThrow_(const std::filesystem::path& path) noexcept
    {
        try {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return !ec;
        }
        catch (...) {
            return false;
        }
    }

    void RemoveFolderNoThrow_(const std::filesystem::path& path) noexcept
    {
        try {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        catch (...) {
        }
    }
}// namespace

bool TimeShift_ViewIndex::HasPrefix_(
    const std::wstring& s,
    const std::wstring& prefix) noexcept
{
    if (s.size() < prefix.size()) {
        return false;
    }

    return std::equal(prefix.begin(), prefix.end(), s.begin());
}

bool TimeShift_ViewIndex::HasSuffix_(
    const std::wstring& s,
    const std::wstring& suffix) noexcept
{
    if (s.size() < suffix.size()) {
        return false;
    }

    return std::equal(
        suffix.rbegin(),
        suffix.rend(),
        s.rbegin());
}

std::wstring TimeShift_ViewIndex::MakeChannelKey(
    int space,
    int channel)
{
    std::wstring key = L"space";
    key += std::to_wstring(space);
    key += L"_ch";
    key += std::to_wstring(channel);
    return key;
}

bool TimeShift_ViewIndex::ParseChannelFolderName_(
    const std::wstring& name,
    int& space,
    int& channel)
{
    space = 0;
    channel = -1;

    if (!HasPrefix_(name, L"space")) {
        return false;
    }

    const std::size_t chPos = name.find(L"_ch");
    if (chPos == std::wstring::npos || chPos <= 5) {
        return false;
    }

    const std::wstring spaceText = name.substr(5, chPos - 5);
    const std::wstring chText = name.substr(chPos + 3);

    if (spaceText.empty() || chText.empty()) {
        return false;
    }

    wchar_t* endPtr = nullptr;

    const long sp = std::wcstol(spaceText.c_str(), &endPtr, 10);
    if (endPtr == spaceText.c_str()) {
        return false;
    }

    endPtr = nullptr;

    const long ch = std::wcstol(chText.c_str(), &endPtr, 10);
    if (endPtr == chText.c_str()) {
        return false;
    }

    if (sp < 0 || ch < 0) {
        return false;
    }

    space = static_cast<int>(sp);
    channel = static_cast<int>(ch);
    return true;
}

bool TimeShift_ViewIndex::ParseSegmentFileName_(
    const std::wstring& fileName,
    std::wstring& startText,
    std::uint64_t& sequence,
    bool& closed)
{
    startText.clear();
    sequence = 0;
    closed = false;

    std::wstring base = fileName;

    if (HasSuffix_(base, L".ts.tmp")) {
        base.erase(base.size() - 7);
        closed = false;
    }
    else if (HasSuffix_(base, L".ts")) {
        base.erase(base.size() - 3);
        closed = true;
    }
    else {
        return false;
    }

    if (!HasPrefix_(base, L"seg_")) {
        return false;
    }

    // seg_YYYYMMDD_HHMMSS_000001
    if (base.size() < 26) {
        return false;
    }

    const std::size_t lastUnderscore = base.find_last_of(L'_');
    if (lastUnderscore == std::wstring::npos ||
        lastUnderscore + 1 >= base.size()) {
        return false;
    }

    const std::wstring seqText = base.substr(lastUnderscore + 1);

    wchar_t* endPtr = nullptr;
    const unsigned long long seq =
        std::wcstoull(seqText.c_str(), &endPtr, 10);

    if (endPtr == seqText.c_str()) {
        return false;
    }

    startText = NormalizeStartText_(base.substr(4, 15));
    sequence = static_cast<std::uint64_t>(seq);
    return true;
}

std::wstring TimeShift_ViewIndex::NormalizeStartText_(
    const std::wstring& raw)
{
    // YYYYMMDD_HHMMSS -> YYYY-MM-DD HH:MM:SS
    if (raw.size() != 15 || raw[8] != L'_') {
        return raw;
    }

    std::wstring s;
    s.reserve(19);

    s.append(raw.substr(0, 4));
    s.push_back(L'-');
    s.append(raw.substr(4, 2));
    s.push_back(L'-');
    s.append(raw.substr(6, 2));
    s.push_back(L' ');
    s.append(raw.substr(9, 2));
    s.push_back(L':');
    s.append(raw.substr(11, 2));
    s.push_back(L':');
    s.append(raw.substr(13, 2));

    return s;
}

bool TimeShift_ViewIndex::ParseDateTimeText_(
    const std::wstring& text,
    std::uint64_t& tick) noexcept
{
    tick = 0;

    // YYYY-MM-DD HH:MM:SS
    if (text.size() < 19) {
        return false;
    }

    SYSTEMTIME st{};
    st.wYear =
        static_cast<WORD>(
            std::wcstol(text.substr(0, 4).c_str(), nullptr, 10));
    st.wMonth =
        static_cast<WORD>(
            std::wcstol(text.substr(5, 2).c_str(), nullptr, 10));
    st.wDay =
        static_cast<WORD>(
            std::wcstol(text.substr(8, 2).c_str(), nullptr, 10));
    st.wHour =
        static_cast<WORD>(
            std::wcstol(text.substr(11, 2).c_str(), nullptr, 10));
    st.wMinute =
        static_cast<WORD>(
            std::wcstol(text.substr(14, 2).c_str(), nullptr, 10));
    st.wSecond =
        static_cast<WORD>(
            std::wcstol(text.substr(17, 2).c_str(), nullptr, 10));

    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) {
        return false;
    }

    ULARGE_INTEGER ui{};
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;

    tick = ui.QuadPart;
    return true;
}

std::wstring TimeShift_ViewIndex::TickToText_(std::uint64_t tick)
{
    ULARGE_INTEGER ui{};
    ui.QuadPart = tick;

    FILETIME ft{};
    ft.dwLowDateTime = ui.LowPart;
    ft.dwHighDateTime = ui.HighPart;

    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) {
        return L"";
    }

    wchar_t buf[32]{};
    swprintf_s(
        buf,
        L"%04u-%02u-%02u %02u:%02u:%02u",
        static_cast<unsigned>(st.wYear),
        static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond));

    return buf;
}

std::uint64_t TimeShift_ViewIndex::AddSeconds_(
    std::uint64_t tick,
    std::uint32_t sec) noexcept
{
    return tick + static_cast<std::uint64_t>(sec) * 10000000ull;
}

std::wstring TimeShift_ViewIndex::FormatBytes(std::uint64_t bytes)
{
    const double gb =
        static_cast<double>(bytes) /
        1024.0 /
        1024.0 /
        1024.0;

    wchar_t buf[64]{};

    if (gb >= 1.0) {
        swprintf_s(buf, L"%.2f GB", gb);
        return buf;
    }

    const double mb =
        static_cast<double>(bytes) /
        1024.0 /
        1024.0;

    swprintf_s(buf, L"%.1f MB", mb);
    return buf;
}

std::wstring TimeShift_ViewIndex::MakeProgramKey_(
    const ProgramSourceEntry& src,
    std::uint64_t startTick)
{
    std::wstring key = MakeChannelKey(src.space, src.channel);
    key += L"_sid";
    key += std::to_wstring(src.serviceId);
    key += L"_eid";
    key += std::to_wstring(src.eventId);
    key += L"_";
    key += std::to_wstring(startTick);
    return key;
}

TimeShift_ViewIndex::BuildResult
TimeShift_ViewIndex::Build(
    const std::filesystem::path& rootFolder,
    std::uint32_t segmentSeconds)
{
    BuildResult out{};
    out.rootFolder = rootFolder;

    if (segmentSeconds < 1u) {
        segmentSeconds = 60u;
    }

    std::error_code ec;

    if (rootFolder.empty()) {
        out.reason = L"rootFolder is empty";
        return out;
    }

    if (!std::filesystem::exists(rootFolder, ec)) {
        out.ok = true;
        return out;
    }

    if (!std::filesystem::is_directory(rootFolder, ec)) {
        out.reason = L"rootFolder is not directory";
        return out;
    }

    for (std::filesystem::directory_iterator dirIt(rootFolder, ec), end;
        !ec && dirIt != end;
        dirIt.increment(ec)) {

        std::error_code dirEc;
        if (!dirIt->is_directory(dirEc)) {
            continue;
        }

        const std::filesystem::path folder = dirIt->path();
        const std::wstring folderName = folder.filename().wstring();

        int space = 0;
        int channel = -1;

        if (!ParseChannelFolderName_(folderName, space, channel)) {
            continue;
        }

        ChannelEntry ch{};
        ch.space = space;
        ch.channel = channel;
        ch.channelKey = MakeChannelKey(space, channel);
        ch.folder = folder;
        ch.status = L"Empty";

        for (std::filesystem::directory_iterator fileIt(folder, ec), fileEnd;
            !ec && fileIt != fileEnd;
            fileIt.increment(ec)) {

            std::error_code fileEc;
            if (!fileIt->is_regular_file(fileEc)) {
                continue;
            }

            const std::filesystem::path p = fileIt->path();
            const std::wstring fileName = p.filename().wstring();

            std::wstring startText;
            std::uint64_t sequence = 0;
            bool closed = false;

            if (!ParseSegmentFileName_(fileName, startText, sequence, closed)) {
                continue;
            }

            std::uint64_t size = 0;
            std::error_code sizeEc;

            size =
                static_cast<std::uint64_t>(
                    std::filesystem::file_size(p, sizeEc));

            if (sizeEc || size == 0) {
                continue;
            }

            SegmentEntry seg{};
            seg.channelKey = ch.channelKey;
            seg.space = space;
            seg.channel = channel;
            seg.sequence = sequence;
            seg.startText = startText;

            std::uint64_t startTick = 0;
            if (ParseDateTimeText_(startText, startTick)) {
                seg.startTick = startTick;
                seg.endTick = AddSeconds_(startTick, segmentSeconds);
                seg.endText = TickToText_(seg.endTick);
            }

            seg.filePath = p;
            seg.bytes = size;
            seg.closed = closed;
            seg.status = closed ? L"Closed" : L"Recording";
            seg.copyAllowed = closed;

            out.segments.push_back(std::move(seg));

            ch.totalBytes += size;
            ++ch.segmentCount;

            if (ch.firstStartText.empty() || startText < ch.firstStartText) {
                ch.firstStartText = startText;
            }

            if (ch.lastStartText.empty() || startText > ch.lastStartText) {
                ch.lastStartText = startText;
            }

            if (!closed) {
                ch.status = L"Recording";
            }
            else if (ch.status != L"Recording") {
                ch.status = L"Active";
            }
        }

        // 0GB、またはセグメント無しフォルダは非表示扱い。
        if (ch.segmentCount == 0 || ch.totalBytes == 0) {
            continue;
        }

        out.channels.push_back(std::move(ch));
    }

    std::sort(
        out.channels.begin(),
        out.channels.end(),
        [](const ChannelEntry& a, const ChannelEntry& b)
        {
            if (a.space != b.space) {
                return a.space < b.space;
            }

            return a.channel < b.channel;
        });

    std::sort(
        out.segments.begin(),
        out.segments.end(),
        [](const SegmentEntry& a, const SegmentEntry& b)
        {
            if (a.channelKey != b.channelKey) {
                return a.channelKey < b.channelKey;
            }

            if (a.startTick != b.startTick) {
                return a.startTick < b.startTick;
            }

            if (a.startText != b.startText) {
                return a.startText < b.startText;
            }

            return a.sequence < b.sequence;
        });

    // 1分セグメントは実際のRotateが数百ms～数秒ずれることがある。
    // endTick = startTick + 60秒 固定のままだと、
    // 次セグメント開始が1秒遅れただけで ViewIndex 上は1秒欠損に見える。
    //
    // 同一チャンネルで次セグメントが予定終了付近にある場合は、
    // 前セグメントの終了を次セグメント開始へ寄せる。
    //
    // 大きな停止穴や容量削除による本物の欠損は埋めない。
    {
        constexpr std::uint64_t kTickPerSec = 10000000ull;
        constexpr std::uint32_t kMaxRotateJitterSec = 5u;

        const std::uint64_t segmentTicks =
            static_cast<std::uint64_t>(segmentSeconds) *
            kTickPerSec;

        const std::uint64_t jitterTicks =
            static_cast<std::uint64_t>(kMaxRotateJitterSec) *
            kTickPerSec;

        for (std::size_t i = 0; i + 1u < out.segments.size(); ++i) {
            SegmentEntry& cur = out.segments[i];
            const SegmentEntry& next = out.segments[i + 1u];

            if (cur.channelKey != next.channelKey) {
                continue;
            }

            if (cur.startTick == 0 ||
                cur.endTick == 0 ||
                next.startTick == 0) {
                continue;
            }

            if (next.startTick <= cur.startTick) {
                continue;
            }

            const std::uint64_t expectedEnd =
                cur.startTick + segmentTicks;

            const bool nearExpectedEnd =
                next.startTick + jitterTicks >= expectedEnd &&
                next.startTick <= expectedEnd + jitterTicks;

            if (!nearExpectedEnd) {
                continue;
            }

            cur.endTick = next.startTick;
            cur.endText = TickToText_(cur.endTick);
        }
    }

    out.ok = true;
    return out;
}

void TimeShift_ViewIndex::BuildProgramsFromSources(
    BuildResult& index,
    const std::vector<ProgramSourceEntry>& programs,
    const ProgramBuildOptions& options)
{
    index.programs.clear();

    ProgramBuildOptions opt = options;

    if (opt.segmentSeconds < 1u) {
        opt.segmentSeconds = 60u;
    }

    if (opt.requiredCoveragePermille > 1000u) {
        opt.requiredCoveragePermille = 1000u;
    }

    for (const auto& src : programs) {
        std::uint64_t startTick = 0;
        std::uint64_t endTick = 0;

        if (!ParseDateTimeText_(src.startText, startTick) ||
            !ParseDateTimeText_(src.endText, endTick) ||
            endTick <= startTick) {
            continue;
        }

        ProgramEntry p{};
        p.space = src.space;
        p.channel = src.channel;
        p.channelKey = MakeChannelKey(src.space, src.channel);

        p.serviceId = src.serviceId;
        p.eventId = src.eventId;
        p.networkId = src.networkId;
        p.tsId = src.tsId;

        p.serviceName = src.serviceName;
        p.title = src.title;

        p.startText = src.startText;
        p.endText = src.endText;

        p.startTick = startTick;
        p.endTick = endTick;

        p.durationSec =
            static_cast<std::uint32_t>(
                (endTick - startTick) / 10000000ull);

        p.programKey = MakeProgramKey_(src, startTick);

        BuildJoinPlan_(index, opt, p);

        index.programs.push_back(std::move(p));
    }

    std::sort(
        index.programs.begin(),
        index.programs.end(),
        [](const ProgramEntry& a, const ProgramEntry& b)
        {
            if (a.channelKey != b.channelKey) {
                return a.channelKey < b.channelKey;
            }

            if (a.startTick != b.startTick) {
                return a.startTick < b.startTick;
            }

            return a.eventId < b.eventId;
        });
}

void TimeShift_ViewIndex::BuildJoinPlan_(
    const BuildResult& index,
    const ProgramBuildOptions& options,
    ProgramEntry& program)
{
    constexpr std::uint64_t kTickPerSec = 10000000ull;

    program.joinSegments.clear();

    program.coveredSec = 0;
    program.coveragePermille = 0;
    program.missingSec = 0;
    program.maxGapSec = 0;
    program.firstMissingStartText.clear();
    program.firstMissingEndText.clear();

    program.boundaryProbeBeforeCount = 0;
    program.boundaryProbeAfterCount = 0;
    program.boundaryProbeMayBeEdgeGap = false;
    program.boundaryProbeBeforeText.clear();
    program.boundaryProbeAfterText.clear();

    program.joinPaddingBeforeCount = 0;
    program.joinPaddingAfterCount = 0;
    program.joinPlanStartText.clear();
    program.joinPlanEndText.clear();

    program.totalBytes = 0;
    program.segmentCount = 0;
    program.viewAllowed = false;
    program.copyAllowed = false;
    program.status.clear();
    program.reason.clear();

    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;

    bool hasRecording = false;

    std::uint32_t segmentSeconds = options.segmentSeconds;
    if (segmentSeconds < 1u) {
        segmentSeconds = 60u;
    }

    const std::uint32_t probeCount =
        options.boundaryProbeSegmentCount;

    const std::uint32_t paddingCount =
        options.joinPaddingSegmentCount;

    const std::uint32_t scanCount =
        (probeCount > paddingCount) ? probeCount : paddingCount;

    const std::uint64_t probeTicks =
        static_cast<std::uint64_t>(segmentSeconds) *
        static_cast<std::uint64_t>(probeCount) *
        kTickPerSec;

    const std::uint64_t paddingTicks =
        static_cast<std::uint64_t>(segmentSeconds) *
        static_cast<std::uint64_t>(paddingCount) *
        kTickPerSec;

    const std::uint64_t scanTicks =
        static_cast<std::uint64_t>(segmentSeconds) *
        static_cast<std::uint64_t>(scanCount) *
        kTickPerSec;

    const std::uint64_t probeStartTick =
        program.startTick > probeTicks
        ? program.startTick - probeTicks
        : 0ull;

    const std::uint64_t probeEndTick =
        (UINT64_MAX - program.endTick > probeTicks)
        ? program.endTick + probeTicks
        : UINT64_MAX;

    const std::uint64_t paddingStartTick =
        program.startTick > paddingTicks
        ? program.startTick - paddingTicks
        : 0ull;

    const std::uint64_t paddingEndTick =
        (UINT64_MAX - program.endTick > paddingTicks)
        ? program.endTick + paddingTicks
        : UINT64_MAX;

    const std::uint64_t scanStartTick =
        program.startTick > scanTicks
        ? program.startTick - scanTicks
        : 0ull;

    const std::uint64_t scanEndTick =
        (UINT64_MAX - program.endTick > scanTicks)
        ? program.endTick + scanTicks
        : UINT64_MAX;

    for (const auto& seg : index.segments) {
        if (seg.channelKey != program.channelKey) {
            continue;
        }

        if (seg.startTick == 0 || seg.endTick == 0) {
            continue;
        }

        const bool overlapsCore =
            !(seg.endTick <= program.startTick ||
                seg.startTick >= program.endTick);

        const bool overlapsProbe =
            !(seg.endTick <= probeStartTick ||
                seg.startTick >= probeEndTick);

        const bool overlapsScan =
            !(seg.endTick <= scanStartTick ||
                seg.startTick >= scanEndTick);

        if (!overlapsScan) {
            continue;
        }

        if (!seg.closed) {
            if (overlapsCore) {
                hasRecording = true;
            }

            // 未確定セグメントは、診断にもコピー計画にも入れない。
            continue;
        }

        if (!overlapsCore) {
            // 番組本体には重ならないが、前後確認範囲には入る。
            // これは「境界ズレかどうか」を見るための診断情報。
            const bool beforeProbe =
                seg.endTick <= program.startTick &&
                seg.endTick > probeStartTick;

            const bool afterProbe =
                seg.startTick >= program.endTick &&
                seg.startTick < probeEndTick;

            if (beforeProbe) {
                ++program.boundaryProbeBeforeCount;

                if (program.boundaryProbeBeforeText.empty()) {
                    program.boundaryProbeBeforeText =
                        seg.startText + L"～" + seg.endText;
                }
            }
            else if (afterProbe) {
                ++program.boundaryProbeAfterCount;

                if (program.boundaryProbeAfterText.empty()) {
                    program.boundaryProbeAfterText =
                        seg.startText + L"～" + seg.endText;
                }
            }

            // 結合保護。
            // coverage / copyAllowed 判定には使わない。
            // ExportJoinedTs() の結合対象にだけ前後セグメントを追加する。
            const bool beforePadding =
                paddingCount > 0 &&
                seg.endTick <= program.startTick &&
                seg.endTick > paddingStartTick;

            const bool afterPadding =
                paddingCount > 0 &&
                seg.startTick >= program.endTick &&
                seg.startTick < paddingEndTick;

            if (beforePadding || afterPadding) {
                JoinSegment js{};
                js.filePath = seg.filePath;
                js.startText = seg.startText;
                js.endText = seg.endText;
                js.startTick = seg.startTick;
                js.endTick = seg.endTick;
                js.bytes = seg.bytes;
                js.sequence = seg.sequence;
                js.closed = seg.closed;

                program.joinSegments.push_back(std::move(js));

                if (beforePadding) {
                    ++program.joinPaddingBeforeCount;
                }
                else {
                    ++program.joinPaddingAfterCount;
                }
            }

            continue;
        }

        JoinSegment js{};
        js.filePath = seg.filePath;
        js.startText = seg.startText;
        js.endText = seg.endText;
        js.startTick = seg.startTick;
        js.endTick = seg.endTick;
        js.bytes = seg.bytes;
        js.sequence = seg.sequence;
        js.closed = seg.closed;

        program.joinSegments.push_back(std::move(js));

        const std::uint64_t a =
            (seg.startTick > program.startTick)
            ? seg.startTick
            : program.startTick;

        const std::uint64_t b =
            (seg.endTick < program.endTick)
            ? seg.endTick
            : program.endTick;

        if (b > a) {
            ranges.push_back({ a, b });
        }
    }

    std::sort(
        program.joinSegments.begin(),
        program.joinSegments.end(),
        [](const JoinSegment& a, const JoinSegment& b)
        {
            if (a.startTick != b.startTick) {
                return a.startTick < b.startTick;
            }

            return a.sequence < b.sequence;
        });

    const std::uint64_t durationTicks =
        program.endTick > program.startTick
        ? program.endTick - program.startTick
        : 0ull;

    if (ranges.empty()) {
        program.coveredSec = 0;
        program.coveragePermille = 0;

        program.missingSec =
            static_cast<std::uint32_t>(
                (durationTicks + kTickPerSec - 1ull) / kTickPerSec);

        program.maxGapSec = program.missingSec;
        program.firstMissingStartText = program.startText;
        program.firstMissingEndText = program.endText;

        program.status = hasRecording ? L"Recording" : L"Deleted";
        program.viewAllowed = false;
        program.copyAllowed = false;

        program.reason =
            hasRecording
            ? L"現在保存中のため、完了後に判定します。"
            : L"対応する保存セグメントがありません。";

        if (program.boundaryProbeBeforeCount > 0 ||
            program.boundaryProbeAfterCount > 0) {
            program.reason += L" boundaryProbe before=";
            program.reason += std::to_wstring(program.boundaryProbeBeforeCount);

            program.reason += L" after=";
            program.reason += std::to_wstring(program.boundaryProbeAfterCount);

            if (!program.boundaryProbeBeforeText.empty()) {
                program.reason += L" beforeSeg=";
                program.reason += program.boundaryProbeBeforeText;
            }

            if (!program.boundaryProbeAfterText.empty()) {
                program.reason += L" afterSeg=";
                program.reason += program.boundaryProbeAfterText;
            }
        }

        return;
    }

    std::sort(
        ranges.begin(),
        ranges.end(),
        [](const auto& a, const auto& b)
        {
            return a.first < b.first;
        });

    std::vector<std::pair<std::uint64_t, std::uint64_t>> mergedRanges;
    mergedRanges.reserve(ranges.size());

    std::uint64_t curA = ranges.front().first;
    std::uint64_t curB = ranges.front().second;

    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first <= curB) {
            if (ranges[i].second > curB) {
                curB = ranges[i].second;
            }
        }
        else {
            mergedRanges.push_back({ curA, curB });
            curA = ranges[i].first;
            curB = ranges[i].second;
        }
    }

    mergedRanges.push_back({ curA, curB });

    std::uint64_t coveredTicks = 0;
    std::uint64_t missingTicks = 0;
    std::uint64_t maxGapTicks = 0;

    std::uint64_t firstGapA = 0;
    std::uint64_t firstGapB = 0;

    std::uint64_t scanTick = program.startTick;

    for (const auto& r : mergedRanges) {
        const std::uint64_t rangeA =
            (r.first > program.startTick)
            ? r.first
            : program.startTick;

        const std::uint64_t rangeB =
            (r.second < program.endTick)
            ? r.second
            : program.endTick;

        if (rangeB <= program.startTick ||
            rangeA >= program.endTick ||
            rangeB <= rangeA) {
            continue;
        }

        if (rangeA > scanTick) {
            const std::uint64_t gapTicks = rangeA - scanTick;

            missingTicks += gapTicks;

            if (firstGapA == 0 && firstGapB == 0) {
                firstGapA = scanTick;
                firstGapB = rangeA;
            }

            if (gapTicks > maxGapTicks) {
                maxGapTicks = gapTicks;
            }
        }

        coveredTicks += rangeB - rangeA;

        if (rangeB > scanTick) {
            scanTick = rangeB;
        }
    }

    if (scanTick < program.endTick) {
        const std::uint64_t gapTicks = program.endTick - scanTick;

        missingTicks += gapTicks;

        if (firstGapA == 0 && firstGapB == 0) {
            firstGapA = scanTick;
            firstGapB = program.endTick;
        }

        if (gapTicks > maxGapTicks) {
            maxGapTicks = gapTicks;
        }
    }

    program.coveredSec =
        static_cast<std::uint32_t>(coveredTicks / kTickPerSec);

    program.missingSec =
        static_cast<std::uint32_t>(
            (missingTicks + kTickPerSec - 1ull) / kTickPerSec);

    program.maxGapSec =
        static_cast<std::uint32_t>(
            (maxGapTicks + kTickPerSec - 1ull) / kTickPerSec);

    if (firstGapA != 0 || firstGapB != 0) {
        program.firstMissingStartText = TickToText_(firstGapA);
        program.firstMissingEndText = TickToText_(firstGapB);
    }
    else {
        program.firstMissingStartText.clear();
        program.firstMissingEndText.clear();
    }

    program.coveragePermille =
        durationTicks > 0
        ? static_cast<std::uint32_t>((coveredTicks * 1000ull) / durationTicks)
        : 0u;

    if (program.coveragePermille > 1000u) {
        program.coveragePermille = 1000u;
    }

    for (const auto& js : program.joinSegments) {
        program.totalBytes += js.bytes;
    }

    program.segmentCount = program.joinSegments.size();

    if (!program.joinSegments.empty()) {
        program.joinPlanStartText = program.joinSegments.front().startText;
        program.joinPlanEndText = program.joinSegments.back().endText;
    }

    const bool firstGapNearStart =
        firstGapA == program.startTick &&
        firstGapB <= program.startTick + probeTicks;

    const bool firstGapNearEnd =
        firstGapB == program.endTick &&
        firstGapA + probeTicks >= program.endTick;

    program.boundaryProbeMayBeEdgeGap =
        (firstGapNearStart && program.boundaryProbeBeforeCount > 0) ||
        (firstGapNearEnd && program.boundaryProbeAfterCount > 0);

    if (hasRecording) {
        program.status = L"Recording";
        program.viewAllowed = false;
        program.copyAllowed = false;
        program.reason = L"現在保存中のセグメントを含むため、閲覧・コピー不可です。";
        return;
    }

    if (program.coveragePermille >= options.requiredCoveragePermille) {
        program.status = L"Available";
        program.viewAllowed = true;
        program.copyAllowed = true;
        program.reason.clear();
        return;
    }

    program.status = L"Partial";
    program.viewAllowed = false;
    program.copyAllowed = false;

    program.reason = L"保存率が100%ではありません。coverage=";
    program.reason += std::to_wstring(program.coveragePermille);
    program.reason += L"/1000";

    program.reason += L" missingSec=";
    program.reason += std::to_wstring(program.missingSec);

    program.reason += L" maxGapSec=";
    program.reason += std::to_wstring(program.maxGapSec);

    if (!program.firstMissingStartText.empty() ||
        !program.firstMissingEndText.empty()) {
        program.reason += L" firstGap=";
        program.reason += program.firstMissingStartText;
        program.reason += L"～";
        program.reason += program.firstMissingEndText;
    }

    program.reason += L" boundaryProbe before=";
    program.reason += std::to_wstring(program.boundaryProbeBeforeCount);

    program.reason += L" after=";
    program.reason += std::to_wstring(program.boundaryProbeAfterCount);

    program.reason += L" edgeGap=";
    program.reason += program.boundaryProbeMayBeEdgeGap ? L"1" : L"0";

    if (!program.boundaryProbeBeforeText.empty()) {
        program.reason += L" beforeSeg=";
        program.reason += program.boundaryProbeBeforeText;
    }

    if (!program.boundaryProbeAfterText.empty()) {
        program.reason += L" afterSeg=";
        program.reason += program.boundaryProbeAfterText;
    }
}

bool TimeShift_ViewIndex::ExportJoinedTs(
    const ProgramEntry& program,
    const std::filesystem::path& outputPath,
    const JoinExportOptions& options,
    std::wstring& reason) noexcept
{
    reason.clear();

    if (!program.copyAllowed) {
        reason = L"コピー不可の番組です。";
        return false;
    }

    if (program.joinSegments.empty()) {
        reason = L"結合対象セグメントがありません。";
        return false;
    }

    const std::filesystem::path cacheRoot =
        options.cacheRoot.empty()
        ? ResolveDefaultCacheRoot_(program, outputPath)
        : options.cacheRoot;

    const std::filesystem::path workFolder =
        MakeWorkFolder_(cacheRoot, program.programKey);

    std::vector<std::filesystem::path> cachedSegments;
    cachedSegments.reserve(program.joinSegments.size());

    try {
        std::error_code ec;
        std::filesystem::create_directories(workFolder, ec);
        if (ec) {
            reason = L"結合キャッシュフォルダの作成に失敗しました: ";
            reason += workFolder.wstring();
            return false;
        }

        // 1. まず全セグメントを cache に退避する。
        //    同一ボリュームなら hardlink、失敗時は copy。
        for (std::size_t i = 0; i < program.joinSegments.size(); ++i) {
            const auto& js = program.joinSegments[i];

            if (!js.closed) {
                reason = L"未確定セグメントが含まれています。";
                if (options.deleteCacheOnFailure) {
                    RemoveFolderNoThrow_(workFolder);
                }
                return false;
            }

            const std::filesystem::path cachedPath =
                MakeNumberedCacheSegmentPath_(workFolder, i);

            std::wstring localReason;

            if (!LinkOrCopyFileToCache_(
                js.filePath,
                cachedPath,
                options.useHardLink,
                localReason)) {

                reason = localReason.empty()
                    ? L"セグメントのキャッシュ退避に失敗しました。"
                    : localReason;

                if (options.deleteCacheOnFailure) {
                    RemoveFolderNoThrow_(workFolder);
                }

                return false;
            }

            cachedSegments.push_back(cachedPath);
        }

        // 2. 退避済みキャッシュだけを使って結合する。
        //    以後、元 seg_*.ts が容量削除されても影響しない。
        std::filesystem::create_directories(outputPath.parent_path(), ec);

        std::filesystem::path tmpOutput = outputPath;
        tmpOutput += L".tmp";

        RemoveFileNoThrow_(tmpOutput);

        std::ofstream ofs(
            tmpOutput,
            std::ios::binary | std::ios::out | std::ios::trunc);

        if (!ofs.is_open()) {
            reason = L"出力一時ファイルを開けません: ";
            reason += tmpOutput.wstring();

            if (options.deleteCacheOnFailure) {
                RemoveFolderNoThrow_(workFolder);
            }

            return false;
        }

        constexpr std::size_t kBufSize = 4u * 1024u * 1024u;
        std::vector<char> buffer(kBufSize);

        for (const auto& cachedPath : cachedSegments) {
            std::ifstream ifs(
                cachedPath,
                std::ios::binary | std::ios::in);

            if (!ifs.is_open()) {
                reason = L"キャッシュ済みセグメントを開けません: ";
                reason += cachedPath.wstring();

                ofs.close();
                RemoveFileNoThrow_(tmpOutput);

                if (options.deleteCacheOnFailure) {
                    RemoveFolderNoThrow_(workFolder);
                }

                return false;
            }

            for (;;) {
                ifs.read(
                    buffer.data(),
                    static_cast<std::streamsize>(buffer.size()));

                const std::streamsize got = ifs.gcount();

                if (got > 0) {
                    ofs.write(buffer.data(), got);

                    if (!ofs.good()) {
                        reason = L"出力一時ファイルへの書き込みに失敗しました。";

                        ifs.close();
                        ofs.close();
                        RemoveFileNoThrow_(tmpOutput);

                        if (options.deleteCacheOnFailure) {
                            RemoveFolderNoThrow_(workFolder);
                        }

                        return false;
                    }
                }

                if (!ifs.good()) {
                    if (ifs.eof()) {
                        break;
                    }

                    reason = L"キャッシュ済みセグメントの読み込みに失敗しました: ";
                    reason += cachedPath.wstring();

                    ifs.close();
                    ofs.close();
                    RemoveFileNoThrow_(tmpOutput);

                    if (options.deleteCacheOnFailure) {
                        RemoveFolderNoThrow_(workFolder);
                    }

                    return false;
                }
            }

            ifs.close();
        }

        ofs.flush();

        const bool writeOk = ofs.good();
        ofs.close();

        if (!writeOk) {
            reason = L"出力一時ファイルの確定に失敗しました。";
            RemoveFileNoThrow_(tmpOutput);

            if (options.deleteCacheOnFailure) {
                RemoveFolderNoThrow_(workFolder);
            }

            return false;
        }

        // 3. output.ts.tmp -> output.ts に確定。
        RemoveFileNoThrow_(outputPath);

        std::filesystem::rename(tmpOutput, outputPath, ec);
        if (ec) {
            reason = L"出力ファイルのリネームに失敗しました: ";
            reason += outputPath.wstring();

            RemoveFileNoThrow_(tmpOutput);

            if (options.deleteCacheOnFailure) {
                RemoveFolderNoThrow_(workFolder);
            }

            return false;
        }

        if (options.deleteCacheOnSuccess) {
            RemoveFolderNoThrow_(workFolder);
        }

        return true;
    }
    catch (...) {
        reason = L"TS結合中に例外が発生しました。";

        if (options.deleteCacheOnFailure) {
            RemoveFolderNoThrow_(workFolder);
        }

        return false;
    }
}

bool TimeShift_ViewIndex::ExportJoinedTs(
    const ProgramEntry& program,
    const std::filesystem::path& outputPath,
    std::wstring& reason) noexcept
{
    JoinExportOptions opt{};
    return ExportJoinedTs(program, outputPath, opt, reason);
}