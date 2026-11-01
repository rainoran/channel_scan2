#pragma once
#ifndef EPG_ARCHIVE_SAVE_HPP
#define EPG_ARCHIVE_SAVE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "DebugManager.h"

// ============================================================
// Epg_archive_save.hpp
// C++20 header-only EIT archive maintenance unit
//
// 方針:
//   - EPG取得処理中には呼ばない
//   - 手動実行、または将来的にEPG完了後のメンテナンスで呼ぶ
//   - 最初は DryRun=1 で対象件数確認のみ
//   - チャンネル別EPGを old 化し、merged EPG は後で再生成する
// ============================================================

// ------------------------------------------------------------
// Compile-time fixed settings
// ------------------------------------------------------------

// 保存モード。
#define EPG_ARCHIVE_SAVE_MODE_MONTH 1
#define EPG_ARCHIVE_SAVE_MODE_YEAR  2

// 0 の場合は無効。1 の場合は有効。
#ifndef EPG_ARCHIVE_SAVE_ENABLE
#define EPG_ARCHIVE_SAVE_ENABLE 0
#endif
// 0 の場合は実際のファイル操作を行う。1 の場合は対象件数の確認のみ。
#ifndef EPG_ARCHIVE_SAVE_DRY_RUN
#define EPG_ARCHIVE_SAVE_DRY_RUN 1
#endif
// 保存モードの選択。月単位 or 年単位。
#ifndef EPG_ARCHIVE_SAVE_MODE
#define EPG_ARCHIVE_SAVE_MODE EPG_ARCHIVE_SAVE_MODE_MONTH
#endif

// Month mode:
// 0 = 当月のみ残す
// 1 = 当月 + 1か月前
// 2 = 当月 + 1か月前 + 2か月前
#ifndef EPG_ARCHIVE_SAVE_KEEP_RECENT_MONTHS
#define EPG_ARCHIVE_SAVE_KEEP_RECENT_MONTHS 2
#endif

// Year mode:
// 2 = 2月になったら前年以前を old 化
#ifndef EPG_ARCHIVE_SAVE_YEAR_START_MONTH
#define EPG_ARCHIVE_SAVE_YEAR_START_MONTH 2
#endif

#ifndef EPG_ARCHIVE_SAVE_MAKE_BACKUP
#define EPG_ARCHIVE_SAVE_MAKE_BACKUP 1
#endif

#ifndef EPG_ARCHIVE_SAVE_OLD_DIR_NAME
#define EPG_ARCHIVE_SAVE_OLD_DIR_NAME L"old"
#endif

static_assert(
    EPG_ARCHIVE_SAVE_MODE == EPG_ARCHIVE_SAVE_MODE_MONTH ||
    EPG_ARCHIVE_SAVE_MODE == EPG_ARCHIVE_SAVE_MODE_YEAR,
    "EPG_ARCHIVE_SAVE_MODE must be EPG_ARCHIVE_SAVE_MODE_MONTH or EPG_ARCHIVE_SAVE_MODE_YEAR.");

static_assert(
    EPG_ARCHIVE_SAVE_KEEP_RECENT_MONTHS >= 0 &&
    EPG_ARCHIVE_SAVE_KEEP_RECENT_MONTHS <= 12,
    "EPG_ARCHIVE_SAVE_KEEP_RECENT_MONTHS must be 0..12.");

static_assert(
    EPG_ARCHIVE_SAVE_YEAR_START_MONTH >= 1 &&
    EPG_ARCHIVE_SAVE_YEAR_START_MONTH <= 12,
    "EPG_ARCHIVE_SAVE_YEAR_START_MONTH must be 1..12.");

namespace EpgArchiveSave
{
    inline constexpr bool kEnabled =
        EPG_ARCHIVE_SAVE_ENABLE != 0;

    inline constexpr bool kDryRun =
        EPG_ARCHIVE_SAVE_DRY_RUN != 0;

    inline constexpr int kMode =
        EPG_ARCHIVE_SAVE_MODE;

    inline constexpr int kKeepRecentMonths =
        EPG_ARCHIVE_SAVE_KEEP_RECENT_MONTHS;

    inline constexpr int kYearStartMonth =
        EPG_ARCHIVE_SAVE_YEAR_START_MONTH;

    inline constexpr bool kMakeBackup =
        EPG_ARCHIVE_SAVE_MAKE_BACKUP != 0;

    struct Date
    {
        int year = 0;
        int month = 0;
        int day = 0;
    };

    struct ArchiveDecision
    {
        bool archive = false;
        std::wstring bucketName;
    };

    struct FileResult
    {
        bool ok = false;
        bool changed = false;
        bool dryRun = kDryRun;

        std::filesystem::path sourcePath;
        std::filesystem::path backupPath;

        std::size_t totalBlocks = 0;
        std::size_t parsedDateBlocks = 0;
        std::size_t keepBlocks = 0;
        std::size_t archiveBlocks = 0;
        std::size_t invalidDateBlocks = 0;
        std::size_t archiveBuckets = 0;

        std::wstring message;
    };

    struct Result
    {
        bool ok = true;
        bool dryRun = kDryRun;

        std::size_t files = 0;
        std::size_t changedFiles = 0;
        std::size_t failedFiles = 0;

        std::size_t totalBlocks = 0;
        std::size_t keepBlocks = 0;
        std::size_t archiveBlocks = 0;
        std::size_t invalidDateBlocks = 0;

        std::vector<FileResult> fileResults;
    };

    struct EventBlock
    {
        std::wstring body;
        bool hasDate = false;
        Date date{};
    };

    inline bool IsValidDate(const Date& d) noexcept
    {
        return d.year >= 1970 &&
            d.year <= 9999 &&
            d.month >= 1 &&
            d.month <= 12 &&
            d.day >= 1 &&
            d.day <= 31;
    }

    inline Date GetLocalToday() noexcept
    {
        std::time_t now = std::time(nullptr);
        std::tm tmv{};

#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        tmv = *std::localtime(&now);
#endif

        Date d{};
        d.year = tmv.tm_year + 1900;
        d.month = tmv.tm_mon + 1;
        d.day = tmv.tm_mday;
        return d;
    }

    inline int ToMonthIndex(const Date& d) noexcept
    {
        return d.year * 12 + (d.month - 1);
    }

    inline std::wstring Pad2(int v)
    {
        std::wstring s = std::to_wstring(v);
        if (s.size() < 2) {
            s.insert(s.begin(), L'0');
        }
        return s;
    }

    inline std::wstring MakeMonthBucketName(const Date& d)
    {
        return std::to_wstring(d.year) + L"-" + Pad2(d.month);
    }

    inline std::wstring MakeYearBucketName(const Date& d)
    {
        return std::to_wstring(d.year);
    }

    inline ArchiveDecision DecideMonthMode(
        const Date& eventDate,
        const Date& today)
    {
        ArchiveDecision r{};

        if (!IsValidDate(eventDate) || !IsValidDate(today)) {
            return r;
        }

        const int eventMonth = ToMonthIndex(eventDate);
        const int currentMonth = ToMonthIndex(today);
        const int keepFromMonth = currentMonth - kKeepRecentMonths;

        r.archive = eventMonth < keepFromMonth;
        r.bucketName = MakeMonthBucketName(eventDate);
        return r;
    }

    inline ArchiveDecision DecideYearMode(
        const Date& eventDate,
        const Date& today)
    {
        ArchiveDecision r{};

        if (!IsValidDate(eventDate) || !IsValidDate(today)) {
            return r;
        }

        int archiveUntilYear = today.year - 2;

        if (today.month >= kYearStartMonth) {
            archiveUntilYear = today.year - 1;
        }

        r.archive = eventDate.year <= archiveUntilYear;
        r.bucketName = MakeYearBucketName(eventDate);
        return r;
    }

    inline ArchiveDecision DecideArchive(
        const Date& eventDate,
        const Date& today)
    {
#if EPG_ARCHIVE_SAVE_MODE == EPG_ARCHIVE_SAVE_MODE_MONTH
        return DecideMonthMode(eventDate, today);
#elif EPG_ARCHIVE_SAVE_MODE == EPG_ARCHIVE_SAVE_MODE_YEAR
        return DecideYearMode(eventDate, today);
#else
        ArchiveDecision r{};
        return r;
#endif
    }

    inline std::wstring TrimLine(std::wstring s)
    {
        while (!s.empty() &&
            (s.back() == L'\r' ||
                s.back() == L'\n' ||
                s.back() == L' ' ||
                s.back() == L'\t')) {
            s.pop_back();
        }

        std::size_t pos = 0;
        while (pos < s.size() &&
            (s[pos] == L' ' ||
                s[pos] == L'\t' ||
                s[pos] == L'\r' ||
                s[pos] == L'\n')) {
            ++pos;
        }

        if (pos > 0) {
            s.erase(0, pos);
        }

        return s;
    }

    inline int HexValue(wchar_t ch) noexcept
    {
        if (ch >= L'0' && ch <= L'9') {
            return static_cast<int>(ch - L'0');
        }
        if (ch >= L'A' && ch <= L'F') {
            return 10 + static_cast<int>(ch - L'A');
        }
        if (ch >= L'a' && ch <= L'f') {
            return 10 + static_cast<int>(ch - L'a');
        }
        return -1;
    }

    inline std::uint64_t FindFirstHexRaw(std::wstring_view line) noexcept
    {
        const std::size_t pos = line.find(L"0x");
        if (pos == std::wstring_view::npos) {
            return 0;
        }

        std::size_t cur = pos + 2;
        std::uint64_t value = 0;
        bool found = false;

        while (cur < line.size()) {
            const int hv = HexValue(line[cur]);
            if (hv < 0) {
                break;
            }

            found = true;
            value = (value << 4) | static_cast<std::uint64_t>(hv & 0x0F);
            ++cur;
        }

        return found ? value : 0;
    }

    inline bool DecodeMjdToDate(int mjd, Date& out)
    {
        long j = static_cast<long>(mjd) + 2400001L + 68569L;
        long c = 4L * j / 146097L;
        j = j - (146097L * c + 3L) / 4L;
        long yy = 4000L * (j + 1L) / 1461001L;
        j = j - 1461L * yy / 4L + 31L;
        long mm = 80L * j / 2447L;
        long dd = j - 2447L * mm / 80L;
        j = mm / 11L;
        mm = mm + 2L - 12L * j;
        yy = 100L * (c - 49L) + yy + j;

        out.year = static_cast<int>(yy);
        out.month = static_cast<int>(mm);
        out.day = static_cast<int>(dd);

        return IsValidDate(out);
    }

    inline bool ParseDateFromStartRawLine(
        std::wstring_view line,
        Date& out)
    {
        const std::uint64_t raw = FindFirstHexRaw(line) & 0xFFFFFFFFFFULL;
        if (raw == 0 || raw == 0xFFFFFFFFFFULL) {
            return false;
        }

        const int mjd =
            static_cast<int>((raw >> 24) & 0xFFFFULL);

        return DecodeMjdToDate(mjd, out);
    }

    inline bool WideToUtf8(
        const std::wstring& text,
        std::string& out)
    {
        out.clear();

#ifdef _WIN32
        if (text.empty()) {
            return true;
        }

        const int bytes = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (bytes <= 0) {
            return false;
        }

        out.assign(static_cast<std::size_t>(bytes), '\0');

        const int written = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            out.data(),
            bytes,
            nullptr,
            nullptr);

        return written > 0;
#else
        out.assign(text.begin(), text.end());
        return true;
#endif
    }

    inline bool Utf8OrAnsiToWide(
        std::string_view src,
        std::wstring& out)
    {
        out.clear();

#ifdef _WIN32
        if (src.empty()) {
            return true;
        }

        int n = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            nullptr,
            0);

        if (n > 0) {
            out.assign(static_cast<std::size_t>(n), L'\0');

            ::MultiByteToWideChar(
                CP_UTF8,
                0,
                src.data(),
                static_cast<int>(src.size()),
                out.data(),
                n);

            return true;
        }

        n = ::MultiByteToWideChar(
            CP_ACP,
            0,
            src.data(),
            static_cast<int>(src.size()),
            nullptr,
            0);

        if (n > 0) {
            out.assign(static_cast<std::size_t>(n), L'\0');

            ::MultiByteToWideChar(
                CP_ACP,
                0,
                src.data(),
                static_cast<int>(src.size()),
                out.data(),
                n);

            return true;
        }

        return false;
#else
        out.assign(src.begin(), src.end());
        return true;
#endif
    }

    inline bool ReadTextFileToWide(
        const std::filesystem::path& path,
        std::wstring& out)
    {
        out.clear();

        try {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) {
                return false;
            }

            ifs.seekg(0, std::ios::end);
            const std::streamoff len = ifs.tellg();
            if (len < 0) {
                return false;
            }

            std::string bytes;
            bytes.resize(static_cast<std::size_t>(len));

            ifs.seekg(0, std::ios::beg);
            if (len > 0) {
                ifs.read(bytes.data(), static_cast<std::streamsize>(len));
            }

            if (bytes.size() >= 3 &&
                static_cast<unsigned char>(bytes[0]) == 0xEF &&
                static_cast<unsigned char>(bytes[1]) == 0xBB &&
                static_cast<unsigned char>(bytes[2]) == 0xBF) {
                bytes.erase(bytes.begin(), bytes.begin() + 3);
            }

            return Utf8OrAnsiToWide(bytes, out);
        }
        catch (...) {
            return false;
        }
    }

    inline bool WriteWideUtf8File(
        const std::filesystem::path& path,
        const std::wstring& text)
    {
        try {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                return false;
            }

            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            ofs.write(reinterpret_cast<const char*>(bom), 3);

            std::string utf8;
            if (!WideToUtf8(text, utf8)) {
                return false;
            }

            if (!utf8.empty()) {
                ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            }

            ofs.flush();
            return ofs.good();
        }
        catch (...) {
            return false;
        }
    }

    inline bool MoveReplace(
        const std::filesystem::path& src,
        const std::filesystem::path& dst) noexcept
    {
        try {
            std::error_code ec;
            std::filesystem::create_directories(dst.parent_path(), ec);

#ifdef _WIN32
            const std::wstring s = src.wstring();
            const std::wstring d = dst.wstring();

            const BOOL ok = ::MoveFileExW(
                s.c_str(),
                d.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

            return ok != FALSE;
#else
            std::filesystem::rename(src, dst, ec);
            return !ec;
#endif
        }
        catch (...) {
            return false;
        }
    }

    inline bool CopyFileReplace(
        const std::filesystem::path& src,
        const std::filesystem::path& dst) noexcept
    {
        try {
            std::error_code ec;
            std::filesystem::create_directories(dst.parent_path(), ec);
            std::filesystem::copy_file(
                src,
                dst,
                std::filesystem::copy_options::overwrite_existing,
                ec);
            return !ec;
        }
        catch (...) {
            return false;
        }
    }

    inline void AppendBlockText(
        std::wstring& dst,
        const std::wstring& block)
    {
        if (block.empty()) {
            return;
        }

        dst += block;

        if (dst.empty() || dst.back() != L'\n') {
            dst += L'\n';
        }

        dst += L'\n';
    }

    inline std::vector<EventBlock> SplitEventBlocks(
        const std::wstring& text)
    {
        std::vector<EventBlock> blocks;

        std::wstring cur;
        bool inEvent = false;

        std::wistringstream iss(text);
        std::wstring line;

        auto flush = [&]() {
            if (cur.empty()) {
                inEvent = false;
                return;
            }

            EventBlock b{};
            b.body = std::move(cur);

            std::wistringstream bs(b.body);
            std::wstring bl;

            while (std::getline(bs, bl)) {
                const std::wstring trimmed = TrimLine(bl);

                if (trimmed.rfind(L"StartTime(raw MJD+BCD40bit):", 0) == 0) {
                    Date d{};
                    if (ParseDateFromStartRawLine(trimmed, d)) {
                        b.hasDate = true;
                        b.date = d;
                    }
                    break;
                }
            }

            blocks.push_back(std::move(b));
            cur.clear();
            inEvent = false;
            };

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }

            const std::wstring trimmed = TrimLine(line);

            if (trimmed.rfind(L"TableID:", 0) == 0) {
                flush();
                inEvent = true;
                cur += line;
                cur += L'\n';
                continue;
            }

            if (!inEvent) {
                continue;
            }

            if (trimmed.empty()) {
                flush();
                continue;
            }

            cur += line;
            cur += L'\n';
        }

        flush();
        return blocks;
    }

    inline std::filesystem::path MakeOldFilePath(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& oldRootDir,
        const std::wstring& bucketName)
    {
        const std::filesystem::path dir =
            oldRootDir / std::filesystem::path(bucketName);

        const std::wstring stem = sourcePath.stem().wstring();
        const std::wstring ext =
            sourcePath.extension().empty()
            ? std::wstring(L".epg")
            : sourcePath.extension().wstring();

        const std::wstring fileName =
            stem + L"_" + bucketName + L"_old" + ext;

        return dir / std::filesystem::path(fileName);
    }

    inline std::filesystem::path ResolveDefaultOldRootDir(
        const std::filesystem::path& sourcePath)
    {
        return sourcePath.parent_path() / std::filesystem::path(EPG_ARCHIVE_SAVE_OLD_DIR_NAME);
    }

    inline bool AppendOldTextAtomically(
        const std::filesystem::path& oldPath,
        const std::wstring& addText)
    {
        if (addText.empty()) {
            return true;
        }

        std::wstring existing;

        if (std::filesystem::exists(oldPath)) {
            if (!ReadTextFileToWide(oldPath, existing)) {
                return false;
            }

            if (!existing.empty() && existing.back() != L'\n') {
                existing += L'\n';
            }

            existing += L'\n';
        }

        existing += addText;

        const std::filesystem::path tmpPath =
            std::filesystem::path(oldPath.wstring() + L".tmp");

        {
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
        }

        if (!WriteWideUtf8File(tmpPath, existing)) {
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
            return false;
        }

        if (!MoveReplace(tmpPath, oldPath)) {
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
            return false;
        }

        return true;
    }

    inline FileResult RunForFile(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& oldRootDir = {},
        Date today = GetLocalToday())
    {
        const auto start = std::chrono::steady_clock::now();

        FileResult r{};
        r.sourcePath = sourcePath;
        r.dryRun = kDryRun;

#if EPG_ARCHIVE_SAVE_ENABLE == 0
        r.ok = true;
        r.changed = false;
        r.message = L"archive disabled";
        return r;
#endif

        std::wstring text;
        if (!ReadTextFileToWide(sourcePath, text)) {
            r.ok = false;
            r.message = L"read failed";

            DebugLogFormat(
                L"[EPG][ARCHIVE][ERROR] read failed: %ls",
                sourcePath.wstring().c_str());

            return r;
        }

        const auto blocks = SplitEventBlocks(text);

        r.totalBlocks = blocks.size();

        if (blocks.empty()) {
            r.ok = true;
            r.changed = false;
            r.message = L"no event blocks";
            return r;
        }

        const std::filesystem::path resolvedOldRoot =
            oldRootDir.empty()
            ? ResolveDefaultOldRootDir(sourcePath)
            : oldRootDir;

        std::wstring keepText;
        std::map<std::wstring, std::wstring> archiveTextByBucket;

        for (const auto& b : blocks) {
            if (!b.hasDate) {
                ++r.invalidDateBlocks;
                ++r.keepBlocks;
                AppendBlockText(keepText, b.body);
                continue;
            }

            ++r.parsedDateBlocks;

            const ArchiveDecision decision =
                DecideArchive(b.date, today);

            if (decision.archive && !decision.bucketName.empty()) {
                ++r.archiveBlocks;
                AppendBlockText(
                    archiveTextByBucket[decision.bucketName],
                    b.body);
            }
            else {
                ++r.keepBlocks;
                AppendBlockText(keepText, b.body);
            }
        }

        r.archiveBuckets = archiveTextByBucket.size();
        r.changed = r.archiveBlocks > 0;

        const auto end = std::chrono::steady_clock::now();
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        DebugLogFormat(
            L"[EPG][ARCHIVE][FILE] dryRun=%d mode=%d path=%ls blocks=%zu parsed=%zu keep=%zu old=%zu invalidDate=%zu buckets=%zu elapsedMs=%lld",
            kDryRun ? 1 : 0,
            kMode,
            sourcePath.wstring().c_str(),
            r.totalBlocks,
            r.parsedDateBlocks,
            r.keepBlocks,
            r.archiveBlocks,
            r.invalidDateBlocks,
            r.archiveBuckets,
            static_cast<long long>(elapsedMs));

#if EPG_ARCHIVE_SAVE_DRY_RUN != 0
        r.ok = true;
        r.message = L"dry run";
        return r;
#else
        if (!r.changed) {
            r.ok = true;
            r.message = L"no archive target";
            return r;
        }

#if EPG_ARCHIVE_SAVE_MAKE_BACKUP != 0
        r.backupPath =
            std::filesystem::path(sourcePath.wstring() + L".archive.bak");

        if (!CopyFileReplace(sourcePath, r.backupPath)) {
            r.ok = false;
            r.message = L"backup failed";

            DebugLogFormat(
                L"[EPG][ARCHIVE][ERROR] backup failed: src=%ls bak=%ls",
                sourcePath.wstring().c_str(),
                r.backupPath.wstring().c_str());

            return r;
        }
#endif

        const std::filesystem::path compactTmpPath =
            std::filesystem::path(sourcePath.wstring() + L".compact.tmp");

        {
            std::error_code ec;
            std::filesystem::remove(compactTmpPath, ec);
        }

        if (!WriteWideUtf8File(compactTmpPath, keepText)) {
            r.ok = false;
            r.message = L"compact tmp write failed";

            std::error_code ec;
            std::filesystem::remove(compactTmpPath, ec);

            DebugLogFormat(
                L"[EPG][ARCHIVE][ERROR] compact tmp write failed: %ls",
                compactTmpPath.wstring().c_str());

            return r;
        }

        for (const auto& kv : archiveTextByBucket) {
            const std::filesystem::path oldPath =
                MakeOldFilePath(sourcePath, resolvedOldRoot, kv.first);

            if (!AppendOldTextAtomically(oldPath, kv.second)) {
                r.ok = false;
                r.message = L"old append failed";

                std::error_code ec;
                std::filesystem::remove(compactTmpPath, ec);

                DebugLogFormat(
                    L"[EPG][ARCHIVE][ERROR] old append failed: old=%ls src=%ls",
                    oldPath.wstring().c_str(),
                    sourcePath.wstring().c_str());

                return r;
            }
        }

        if (!MoveReplace(compactTmpPath, sourcePath)) {
            r.ok = false;
            r.message = L"source replace failed";

            std::error_code ec;
            std::filesystem::remove(compactTmpPath, ec);

#if EPG_ARCHIVE_SAVE_MAKE_BACKUP != 0
            if (!r.backupPath.empty()) {
                (void)CopyFileReplace(r.backupPath, sourcePath);
            }
#endif

            DebugLogFormat(
                L"[EPG][ARCHIVE][ERROR] source replace failed: %ls",
                sourcePath.wstring().c_str());

            return r;
        }

        r.ok = true;
        r.message = L"archived";

        DebugLogFormat(
            L"[EPG][ARCHIVE][COMMIT] path=%ls keep=%zu old=%zu buckets=%zu backup=%ls",
            sourcePath.wstring().c_str(),
            r.keepBlocks,
            r.archiveBlocks,
            r.archiveBuckets,
            r.backupPath.empty() ? L"" : r.backupPath.wstring().c_str());

        return r;
#endif
    }

    inline Result RunForFiles(
        const std::vector<std::filesystem::path>& sourceFiles,
        const std::filesystem::path& oldRootDir = {},
        Date today = GetLocalToday())
    {
        const auto start = std::chrono::steady_clock::now();

        Result result{};
        result.dryRun = kDryRun;

        DebugLogFormat(
            L"[EPG][ARCHIVE][START] enable=%d dryRun=%d mode=%d files=%zu keepRecentMonths=%d yearStartMonth=%d",
            kEnabled ? 1 : 0,
            kDryRun ? 1 : 0,
            kMode,
            sourceFiles.size(),
            kKeepRecentMonths,
            kYearStartMonth);

        for (const auto& file : sourceFiles) {
            ++result.files;

            FileResult fr =
                RunForFile(file, oldRootDir, today);

            result.totalBlocks += fr.totalBlocks;
            result.keepBlocks += fr.keepBlocks;
            result.archiveBlocks += fr.archiveBlocks;
            result.invalidDateBlocks += fr.invalidDateBlocks;

            if (!fr.ok) {
                ++result.failedFiles;
                result.ok = false;
            }

            if (fr.changed) {
                ++result.changedFiles;
            }

            result.fileResults.push_back(std::move(fr));
        }

        const auto end = std::chrono::steady_clock::now();
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        DebugLogFormat(
            L"[EPG][ARCHIVE][END] ok=%d dryRun=%d files=%zu changed=%zu failed=%zu blocks=%zu keep=%zu old=%zu invalidDate=%zu elapsedMs=%lld",
            result.ok ? 1 : 0,
            result.dryRun ? 1 : 0,
            result.files,
            result.changedFiles,
            result.failedFiles,
            result.totalBlocks,
            result.keepBlocks,
            result.archiveBlocks,
            result.invalidDateBlocks,
            static_cast<long long>(elapsedMs));

        return result;
    }
}

#endif