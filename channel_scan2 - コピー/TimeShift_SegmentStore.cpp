#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TimeShift_SegmentStore.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <ctime>

TimeShift_SegmentStore::~TimeShift_SegmentStore() noexcept
{
    StopQuotaCleaner();
    StopAll();
}

void TimeShift_SegmentStore::Configure(const Settings& settings)
{
    std::lock_guard<std::mutex> lock(mutex_);

    settings_ = settings;

    if (settings_.rootFolder.empty()) {
        settings_.rootFolder = L"timeshift";
    }

    // maxBytes=0 は容量監視無効として扱う。
// テスト時に小さい容量を使えるよう、極端な下限補正はしない。
    if (settings_.maxBytes != 0 &&
        settings_.maxBytes < 1024ull * 1024ull) {
        settings_.maxBytes = 1024ull * 1024ull;
    }

    if (settings_.highWatermarkPercent < 10) {
        settings_.highWatermarkPercent = 10;
    }
    if (settings_.highWatermarkPercent > 95) {
        settings_.highWatermarkPercent = 95;
    }

    if (settings_.lowWatermarkPercent < 1) {
        settings_.lowWatermarkPercent = 1;
    }
    if (settings_.lowWatermarkPercent >= settings_.highWatermarkPercent) {
        settings_.lowWatermarkPercent = settings_.highWatermarkPercent - 1;
    }

    if (settings_.segmentSeconds == 0) {
        settings_.segmentSeconds = 60;
    }

    if (settings_.segmentMaxBytes < 16ull * 1024ull * 1024ull) {
        settings_.segmentMaxBytes = 16ull * 1024ull * 1024ull;
    }

    if (settings_.packetSize == 0) {
        settings_.packetSize = 188;
    }

    if (settings_.quotaCleanerIntervalSec == 0) {
        settings_.quotaCleanerIntervalSec = 15;
    }
    if (settings_.quotaCleanerIntervalSec > 3600) {
        settings_.quotaCleanerIntervalSec = 3600;
    }

    if (settings_.quotaCleanerProtectRecentSec < 60) {
        settings_.quotaCleanerProtectRecentSec = 60;
    }
    if (settings_.quotaCleanerProtectRecentSec > 24u * 60u * 60u) {
        settings_.quotaCleanerProtectRecentSec = 24u * 60u * 60u;
    }

    if (settings_.quotaCleanerMaxDeleteSegmentsPerPass == 0) {
        settings_.quotaCleanerMaxDeleteSegmentsPerPass = 16;
    }
    if (settings_.quotaCleanerMaxDeleteSegmentsPerPass > 256) {
        settings_.quotaCleanerMaxDeleteSegmentsPerPass = 256;
    }
}

bool TimeShift_SegmentStore::StartChannel(const ChannelKey& key)
{
    if (!IsValidKey_(key)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::wstring mapKey = MakeMapKey_(key);
        Session& s = sessions_[mapKey];

        if (s.active) {
            // すでに開始済みでも Cleaner は起動要求しておく。
        }
        else {
            s.key = key;
            s.folder = MakeChannelFolderPath_(key);

            std::error_code ec;
            std::filesystem::create_directories(s.folder, ec);
            if (ec) {
                return false;
            }

            RecoverChannelLocked_(s);

            if (!OpenNewSegmentLocked_(s)) {
                s.active = false;
                return false;
            }

            s.active = true;
        }
    }

    StartQuotaCleaner();
    RequestQuotaCleanup();

    return true;
}

void TimeShift_SegmentStore::StopChannel(const ChannelKey& key) noexcept
{
    bool shouldStopCleaner = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::wstring mapKey = MakeMapKey_(key);
        auto it = sessions_.find(mapKey);
        if (it == sessions_.end()) {
            return;
        }

        Session& s = it->second;

        CloseCurrentSegmentLocked_(s);

        // Stop時も容量削除は実行しない。
        // 停止処理に rootFolder scan/delete が混ざると、
        // 設定再適用時の Stop→Start→Reload と衝突しやすい。
        WriteManifestLocked_(s);

        s.active = false;

        bool anyActive = false;
        for (const auto& pair : sessions_) {
            if (pair.second.active) {
                anyActive = true;
                break;
            }
        }

        shouldStopCleaner = !anyActive;
    }

    if (shouldStopCleaner) {
        StopQuotaCleaner();
    }
}

void TimeShift_SegmentStore::StopAll() noexcept
{
    StopQuotaCleaner();

    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : sessions_) {
        Session& s = pair.second;

        CloseCurrentSegmentLocked_(s);
        WriteManifestLocked_(s);

        s.active = false;
    }

    // StopAll時も容量削除は実行しない。
}

bool TimeShift_SegmentStore::WriteBytes(
    const ChannelKey& key,
    const void* data,
    std::size_t bytes) noexcept
{
    if (data == nullptr || bytes == 0 || !IsValidKey_(key)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const std::wstring mapKey = MakeMapKey_(key);
    auto it = sessions_.find(mapKey);
    if (it == sessions_.end()) {
        return false;
    }

    Session& s = it->second;
    if (!s.active) {
        return false;
    }

    if (!s.currentFile.is_open()) {
        if (!OpenNewSegmentLocked_(s)) {
            s.dropBytes += static_cast<std::uint64_t>(bytes);
            return false;
        }
    }

    s.currentFile.write(
        static_cast<const char*>(data),
        static_cast<std::streamsize>(bytes));

    if (!s.currentFile.good()) {
        s.dropBytes += static_cast<std::uint64_t>(bytes);
        return false;
    }

    s.currentBytes += static_cast<std::uint64_t>(bytes);

    if (settings_.packetSize > 0) {
        s.currentPackets +=
            static_cast<std::uint64_t>(bytes) /
            static_cast<std::uint64_t>(settings_.packetSize);
    }

    const std::uint64_t nowMs = GetTickMs_();
    const std::uint64_t elapsedMs =
        (nowMs >= s.currentStartTickMs)
        ? (nowMs - s.currentStartTickMs)
        : 0;

    const bool needRotateByTime =
        elapsedMs >= static_cast<std::uint64_t>(settings_.segmentSeconds) * 1000ull;

    const bool needRotateBySize =
        s.currentBytes >= settings_.segmentMaxBytes;

    if (needRotateByTime || needRotateBySize) {
        CloseCurrentSegmentLocked_(s);

        // Writer hot path では削除しない。
        // ここでは次セグメントを開くだけ。
        const bool opened = OpenNewSegmentLocked_(s);

        // Cleanerへ削除要求だけを出す。
        // RequestQuotaCleanup() は atomic + notify のみで重い処理をしない。
        RequestQuotaCleanup();

        if (!opened) {
            s.dropBytes += static_cast<std::uint64_t>(bytes);
            return false;
        }
    }

    return true;
}

bool TimeShift_SegmentStore::Rotate(const ChannelKey& key) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    const std::wstring mapKey = MakeMapKey_(key);
    auto it = sessions_.find(mapKey);
    if (it == sessions_.end()) {
        return false;
    }

    Session& s = it->second;
    if (!s.active) {
        return false;
    }

    CloseCurrentSegmentLocked_(s);

    // 手動Rotateでも Writer 側では容量削除しない。
    // 削除要求だけ Cleaner に渡す。
    const bool opened = OpenNewSegmentLocked_(s);

    RequestQuotaCleanup();

    return opened;
}

std::vector<TimeShift_SegmentStore::SegmentInfo>
TimeShift_SegmentStore::ListSegments(const ChannelKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const std::wstring mapKey = MakeMapKey_(key);
    auto it = sessions_.find(mapKey);
    if (it != sessions_.end()) {
        SortSegmentsLocked_(it->second);
        return it->second.segments;
    }

    Session temp{};
    temp.key = key;
    temp.folder = MakeChannelFolderPath_(key);

    RecoverChannelLocked_(temp);
    SortSegmentsLocked_(temp);

    return temp.segments;
}

TimeShift_SegmentStore::Status
TimeShift_SegmentStore::GetStatus(const ChannelKey& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    Status st{};
    
    st.key = key;
    st.channelFolder = MakeChannelFolderPath_(key);

    const std::wstring mapKey = MakeMapKey_(key);
    auto it = sessions_.find(mapKey);
    if (it == sessions_.end()) {
        return st;
    }

    const Session& s = it->second;

    st.active = s.active;
    st.channelFolder = s.folder;
    st.currentTempPath = s.currentTempPath;
    st.currentFinalPath = s.currentFinalPath;
    st.currentBytes = s.currentBytes;
    st.currentPackets = s.currentPackets;
    st.dropBytes = s.dropBytes;
    st.closedSegmentCount = s.segments.size();
    st.totalClosedBytes = CalcClosedBytesLocked_(s);

    return st;
}

TimeShift_SegmentStore::QuotaStatus
TimeShift_SegmentStore::EnforceQuota() noexcept
{
    return EnforceQuotaFromCleaner_();
}

bool TimeShift_SegmentStore::StartQuotaCleaner() noexcept
{
    bool expected = false;

    if (!quotaCleanerRunning_.compare_exchange_strong(
        expected,
        true,
        std::memory_order_acq_rel)) {
        return true;
    }

    quotaCleanerStopRequested_.store(false, std::memory_order_release);
    quotaCleanupRequested_.store(true, std::memory_order_release);

    try {
        quotaCleanerThread_ =
            std::thread(&TimeShift_SegmentStore::QuotaCleanerLoop_, this);
    }
    catch (...) {
        quotaCleanerRunning_.store(false, std::memory_order_release);
        quotaCleanerStopRequested_.store(true, std::memory_order_release);
        return false;
    }

    quotaCleanerCv_.notify_one();
    return true;
}

void TimeShift_SegmentStore::StopQuotaCleaner() noexcept
{
    quotaCleanerStopRequested_.store(true, std::memory_order_release);
    quotaCleanerCv_.notify_all();

    if (quotaCleanerThread_.joinable()) {
        if (quotaCleanerThread_.get_id() != std::this_thread::get_id()) {
            quotaCleanerThread_.join();
        }
        else {
            quotaCleanerThread_.detach();
        }
    }

    quotaCleanerRunning_.store(false, std::memory_order_release);
    quotaCleanupRequested_.store(false, std::memory_order_release);
}

void TimeShift_SegmentStore::RequestQuotaCleanup() noexcept
{
    quotaCleanupRequested_.store(true, std::memory_order_release);
    quotaCleanerCv_.notify_one();
}

void TimeShift_SegmentStore::QuotaCleanerLoop_() noexcept
{
    for (;;) {
        std::uint32_t intervalSec = 15;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            intervalSec = settings_.quotaCleanerIntervalSec;
            if (intervalSec == 0) {
                intervalSec = 15;
            }
        }

        {
            std::unique_lock<std::mutex> lock(quotaCleanerMutex_);

            quotaCleanerCv_.wait_for(
                lock,
                std::chrono::seconds(intervalSec),
                [this]()
                {
                    return
                        quotaCleanerStopRequested_.load(std::memory_order_acquire) ||
                        quotaCleanupRequested_.load(std::memory_order_acquire);
                });
        }

        if (quotaCleanerStopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        quotaCleanupRequested_.store(false, std::memory_order_release);

        EnforceQuotaFromCleaner_();
    }

    quotaCleanerRunning_.store(false, std::memory_order_release);
}

bool TimeShift_SegmentStore::ResetChannel(const ChannelKey& key) noexcept
{
    // Reset中に Cleaner が root/channel folder を触らないよう先に止める。
    StopQuotaCleaner();

    StopChannel(key);

    std::lock_guard<std::mutex> lock(mutex_);

    const std::filesystem::path folder = MakeChannelFolderPath_(key);

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);

    sessions_.erase(MakeMapKey_(key));

    return !ec;
}

std::wstring TimeShift_SegmentStore::MakeChannelFolderName(const ChannelKey& key)
{
    std::wstring s = L"space";
    s += std::to_wstring(key.space);
    s += L"_ch";
    s += std::to_wstring(key.channel);
    return s;
}

std::wstring TimeShift_SegmentStore::MakeNowText_()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t buf[32]{};
    _snwprintf_s(
        buf,
        _countof(buf),
        _TRUNCATE,
        L"%04u%02u%02u_%02u%02u%02u",
        static_cast<unsigned>(st.wYear),
        static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond));

    return buf;
}

std::wstring TimeShift_SegmentStore::MakeProtectCutoffText_(
    std::uint32_t protectRecentSec) noexcept
{
    try {
        using Clock = std::chrono::system_clock;

        const auto now = Clock::now();
        const auto cutoff =
            now - std::chrono::seconds(
                static_cast<std::int64_t>(protectRecentSec));

        const std::time_t tt = Clock::to_time_t(cutoff);

        std::tm tmLocal{};
        if (localtime_s(&tmLocal, &tt) != 0) {
            return L"";
        }

        wchar_t buf[32]{};
        _snwprintf_s(
            buf,
            _countof(buf),
            _TRUNCATE,
            L"%04d%02d%02d_%02d%02d%02d",
            tmLocal.tm_year + 1900,
            tmLocal.tm_mon + 1,
            tmLocal.tm_mday,
            tmLocal.tm_hour,
            tmLocal.tm_min,
            tmLocal.tm_sec);

        return buf;
    }
    catch (...) {
        return L"";
    }
}

std::uint64_t TimeShift_SegmentStore::GetTickMs_() noexcept
{
    return static_cast<std::uint64_t>(GetTickCount64());
}

std::wstring TimeShift_SegmentStore::MakeMapKey_(const ChannelKey& key)
{
    return std::to_wstring(key.space) + L":" + std::to_wstring(key.channel);
}

bool TimeShift_SegmentStore::IsValidKey_(const ChannelKey& key) noexcept
{
    return key.space >= 0 && key.channel >= 0;
}

std::mutex& TimeShift_SegmentStore::RootQuotaMutex_() noexcept
{
    static std::mutex m;
    return m;
}

std::filesystem::path
TimeShift_SegmentStore::MakeChannelFolderPath_(const ChannelKey& key) const
{
    return settings_.rootFolder / MakeChannelFolderName(key);
}

std::filesystem::path
TimeShift_SegmentStore::MakeManifestPath_(const Session& s) const
{
    return s.folder / L"manifest.tsv";
}

bool TimeShift_SegmentStore::RecoverChannelLocked_(Session& s) noexcept
{
    s.segments.clear();
    s.nextSequence = 1;

    std::error_code ec;
    std::filesystem::create_directories(s.folder, ec);
    if (ec) {
        return false;
    }

    // 前回クラッシュ時の中途半端ファイルを削除。
    for (const auto& entry : std::filesystem::directory_iterator(s.folder, ec)) {
        if (ec) {
            break;
        }

        const std::filesystem::path p = entry.path();
        const std::wstring ext = p.extension().wstring();

        if (ext == L".tmp" || ext == L".delete") {
            std::error_code rmEc;
            std::filesystem::remove(p, rmEc);
        }
    }

    ec.clear();

    for (const auto& entry : std::filesystem::directory_iterator(s.folder, ec)) {
        if (ec) {
            break;
        }

        if (!entry.is_regular_file(ec)) {
            continue;
        }

        const std::filesystem::path p = entry.path();

        if (p.extension() != L".ts") {
            continue;
        }

        const std::wstring name = p.filename().wstring();
        if (!HasPrefix_(name, L"seg_")) {
            continue;
        }

        const std::uint64_t seq = ParseSequenceFromSegmentName_(p);
        if (seq == 0) {
            continue;
        }

        std::uint64_t bytes = 0;
        std::error_code sizeEc;
        bytes = static_cast<std::uint64_t>(std::filesystem::file_size(p, sizeEc));
        if (sizeEc) {
            bytes = 0;
        }

        SegmentInfo seg{};
        seg.sequence = seq;
        seg.filePath = p;
        seg.bytes = bytes;
        seg.packetCount =
            (settings_.packetSize > 0)
            ? (bytes / static_cast<std::uint64_t>(settings_.packetSize))
            : 0;
        seg.closed = true;

        const std::wstring stem = p.stem().wstring();
        // seg_YYYYMMDD_HHMMSS_000001
        if (stem.size() >= 19) {
            seg.startText = stem.substr(4, 15);
        }

        s.segments.push_back(seg);

        if (seq >= s.nextSequence) {
            s.nextSequence = seq + 1;
        }
    }

    SortSegmentsLocked_(s);
    WriteManifestLocked_(s);

    return true;
}

bool TimeShift_SegmentStore::OpenNewSegmentLocked_(Session& s) noexcept
{
    if (s.currentFile.is_open()) {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(s.folder, ec);
    if (ec) {
        return false;
    }

    const std::wstring now = MakeNowText_();

    wchar_t seqBuf[32]{};
    _snwprintf_s(
        seqBuf,
        _countof(seqBuf),
        _TRUNCATE,
        L"%06llu",
        static_cast<unsigned long long>(s.nextSequence));

    std::wstring base = L"seg_";
    base += now;
    base += L"_";
    base += seqBuf;

    s.currentFinalPath = s.folder / (base + L".ts");
    s.currentTempPath = s.folder / (base + L".ts.tmp");

    s.currentFile.open(
        s.currentTempPath,
        std::ios::binary | std::ios::out | std::ios::trunc);

    if (!s.currentFile.is_open()) {
        s.currentTempPath.clear();
        s.currentFinalPath.clear();
        return false;
    }

    s.currentStartText = now;
    s.currentStartTickMs = GetTickMs_();
    s.currentBytes = 0;
    s.currentPackets = 0;

    ++s.nextSequence;

    return true;
}

bool TimeShift_SegmentStore::CloseCurrentSegmentLocked_(Session& s) noexcept
{
    if (!s.currentFile.is_open()) {
        return true;
    }

    s.currentFile.flush();
    s.currentFile.close();

    if (s.currentBytes == 0) {
        std::error_code rmEc;
        std::filesystem::remove(s.currentTempPath, rmEc);

        s.currentTempPath.clear();
        s.currentFinalPath.clear();
        s.currentStartText.clear();
        s.currentBytes = 0;
        s.currentPackets = 0;
        return true;
    }

    std::error_code ec;

    if (std::filesystem::exists(s.currentFinalPath, ec)) {
        std::filesystem::remove(s.currentFinalPath, ec);
        ec.clear();
    }

    std::filesystem::rename(s.currentTempPath, s.currentFinalPath, ec);
    if (ec) {
        // rename失敗時はtmpを残さず削除して、次のセグメントへ進める。
        std::error_code rmEc;
        std::filesystem::remove(s.currentTempPath, rmEc);

        s.currentTempPath.clear();
        s.currentFinalPath.clear();
        s.currentStartText.clear();
        s.currentBytes = 0;
        s.currentPackets = 0;
        return false;
    }

    SegmentInfo seg{};
    seg.sequence = s.nextSequence > 0 ? (s.nextSequence - 1) : 0;
    seg.startText = s.currentStartText;
    seg.endText = MakeNowText_();
    seg.filePath = s.currentFinalPath;
    seg.bytes = s.currentBytes;
    seg.packetCount = s.currentPackets;
    seg.closed = true;

    s.segments.push_back(seg);
    SortSegmentsLocked_(s);

    s.currentTempPath.clear();
    s.currentFinalPath.clear();
    s.currentStartText.clear();
    s.currentBytes = 0;
    s.currentPackets = 0;

    WriteManifestLocked_(s);

    return true;
}

bool TimeShift_SegmentStore::WriteManifestLocked_(const Session& s) noexcept
{
    std::error_code ec;
    std::filesystem::create_directories(s.folder, ec);
    if (ec) {
        return false;
    }

    const std::filesystem::path manifest = MakeManifestPath_(s);
    const std::filesystem::path tmp = s.folder / L"manifest.tsv.tmp";

    {
        std::wofstream ofs(tmp, std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) {
            return false;
        }

        ofs << L"sequence\tstart\tend\tfileName\tbytes\tpacketCount\tclosed\n";

        for (const auto& seg : s.segments) {
            ofs
                << seg.sequence << L"\t"
                << seg.startText << L"\t"
                << seg.endText << L"\t"
                << seg.filePath.filename().wstring() << L"\t"
                << seg.bytes << L"\t"
                << seg.packetCount << L"\t"
                << (seg.closed ? 1 : 0) << L"\n";
        }

        ofs.flush();
        ofs.close();
    }

    ec.clear();

    if (std::filesystem::exists(manifest, ec)) {
        std::filesystem::remove(manifest, ec);
        ec.clear();
    }

    std::filesystem::rename(tmp, manifest, ec);
    if (ec) {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }

    return true;
}

TimeShift_SegmentStore::QuotaStatus
TimeShift_SegmentStore::EnforceQuotaFromCleaner_() noexcept
{
    QuotaStatus result{};

    try {
        Settings settings{};

        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings = settings_;
        }

        result.rootFolder = settings.rootFolder;
        result.maxBytes = settings.maxBytes;

        if (settings.maxBytes == 0) {
            result.ok = true;
            result.reason = L"quota disabled";
            return result;
        }

        result.highBytes =
            settings.maxBytes *
            static_cast<std::uint64_t>(settings.highWatermarkPercent) /
            100ull;

        result.lowBytes =
            settings.maxBytes *
            static_cast<std::uint64_t>(settings.lowWatermarkPercent) /
            100ull;

        std::unique_lock<std::mutex> rootQuotaLock(
            RootQuotaMutex_(),
            std::try_to_lock);

        if (!rootQuotaLock.owns_lock()) {
            result.ok = true;
            result.reason = L"quota cleanup busy; skipped";
            return result;
        }

        CleanupDeleteLeftoversForCleaner_(settings, result);

        std::vector<RootSegmentInfo> segments;
        if (!ScanRootSegmentsForCleaner_(settings, segments, result)) {
            result.ok = false;
            if (result.reason.empty()) {
                result.reason = L"rootFolder scan failed";
            }
            return result;
        }

        result.overHighWatermark =
            result.totalBefore > result.highBytes;

        if (!result.overHighWatermark) {
            result.totalAfter = result.totalBefore;
            result.reducedToLowWatermark =
                result.totalAfter <= result.lowBytes;
            result.ok = true;
            return result;
        }

        std::sort(
            segments.begin(),
            segments.end(),
            [](const RootSegmentInfo& a, const RootSegmentInfo& b)
            {
                if (a.startText != b.startText) {
                    return a.startText < b.startText;
                }

                if (a.sequence != b.sequence) {
                    return a.sequence < b.sequence;
                }

                return a.filePath.wstring() < b.filePath.wstring();
            });

        const std::wstring protectCutoff =
            MakeProtectCutoffText_(settings.quotaCleanerProtectRecentSec);

        std::uint64_t total = result.totalBefore;
        std::vector<std::filesystem::path> touchedFolders;

        const auto addTouchedFolder =
            [&touchedFolders](const std::filesystem::path& folder)
            {
                const std::wstring key = MakePathKey_(folder);

                for (const auto& f : touchedFolders) {
                    if (MakePathKey_(f) == key) {
                        return;
                    }
                }

                touchedFolders.push_back(folder);
            };

        for (const auto& seg : segments) {
            if (total <= result.lowBytes) {
                break;
            }

            if (result.deletedSegments >=
                static_cast<std::size_t>(
                    settings.quotaCleanerMaxDeleteSegmentsPerPass)) {
                break;
            }

            // 最新保護範囲は削除しない。
            // startText は seg_YYYYMMDD_HHMMSS 由来なので文字列比較で時系列になる。
            if (!protectCutoff.empty() &&
                !seg.startText.empty() &&
                seg.startText >= protectCutoff) {
                continue;
            }

            if (DeleteRootSegmentForCleaner_(seg, result)) {
                addTouchedFolder(seg.channelFolder);

                if (total >= seg.bytes) {
                    total -= seg.bytes;
                }
                else {
                    total = 0;
                }
            }
        }

        result.totalAfter = total;
        result.reducedToLowWatermark =
            result.totalAfter <= result.lowBytes;

        if (!touchedFolders.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            RefreshManifestsAfterRootDeleteLocked_(
                touchedFolders,
                result);
        }

        result.ok = result.failedDeletes == 0;

        if (!result.ok && result.reason.empty()) {
            result.reason = L"一部セグメントの削除に失敗しました。";
        }

        return result;
    }
    catch (...) {
        result.ok = false;
        result.reason = L"quota cleaner exception";
        return result;
    }
}

TimeShift_SegmentStore::QuotaStatus
TimeShift_SegmentStore::EnforceQuotaLocked_() noexcept
{
    QuotaStatus result{};

    result.rootFolder = settings_.rootFolder;
    result.maxBytes = settings_.maxBytes;

    if (settings_.maxBytes == 0) {
        result.ok = true;
        result.reason = L"quota disabled";
        return result;
    }

    // 複数チューナーの store_ が同じ rootFolder を見るため、
    // rootFolder 全体の容量削除はグローバルに1本化する。
    //
    // 重要:
    // ここでは待たない。
    // すでに別スレッドが削除中なら、この回の容量削除はスキップする。
    // Writer側を削除待ちで長時間止めないため。
    std::unique_lock<std::mutex> rootQuotaLock(
        RootQuotaMutex_(),
        std::try_to_lock);

    if (!rootQuotaLock.owns_lock()) {
        result.ok = true;
        result.reason = L"quota cleanup busy; skipped";

        result.highBytes =
            settings_.maxBytes *
            static_cast<std::uint64_t>(settings_.highWatermarkPercent) /
            100ull;

        result.lowBytes =
            settings_.maxBytes *
            static_cast<std::uint64_t>(settings_.lowWatermarkPercent) /
            100ull;

        return result;
    }

    result.highBytes =
        settings_.maxBytes *
        static_cast<std::uint64_t>(settings_.highWatermarkPercent) /
        100ull;

    result.lowBytes =
        settings_.maxBytes *
        static_cast<std::uint64_t>(settings_.lowWatermarkPercent) /
        100ull;

    CleanupDeleteLeftoversLocked_(result);

    std::vector<RootSegmentInfo> segments;
    if (!ScanRootSegmentsLocked_(segments, result)) {
        result.ok = false;
        if (result.reason.empty()) {
            result.reason = L"rootFolder scan failed";
        }
        return result;
    }

    result.overHighWatermark = result.totalBefore > result.highBytes;

    if (!result.overHighWatermark) {
        result.totalAfter = result.totalBefore;
        result.reducedToLowWatermark = result.totalAfter <= result.lowBytes;
        result.ok = true;
        return result;
    }

    std::sort(
        segments.begin(),
        segments.end(),
        [](const RootSegmentInfo& a, const RootSegmentInfo& b)
        {
            if (a.startText != b.startText) {
                return a.startText < b.startText;
            }

            if (a.sequence != b.sequence) {
                return a.sequence < b.sequence;
            }

            return a.filePath.wstring() < b.filePath.wstring();
        });

    std::uint64_t total = result.totalBefore;
    std::vector<std::filesystem::path> touchedFolders;

    const auto addTouchedFolder =
        [&touchedFolders](const std::filesystem::path& folder)
        {
            const std::wstring key = MakePathKey_(folder);

            for (const auto& f : touchedFolders) {
                if (MakePathKey_(f) == key) {
                    return;
                }
            }

            touchedFolders.push_back(folder);
        };

    for (const auto& seg : segments) {
        if (total <= result.lowBytes) {
            break;
        }

        if (DeleteRootSegmentLocked_(seg, result)) {
            addTouchedFolder(seg.channelFolder);

            if (total >= seg.bytes) {
                total -= seg.bytes;
            }
            else {
                total = 0;
            }
        }
    }

    result.totalAfter = total;
    result.reducedToLowWatermark = result.totalAfter <= result.lowBytes;

    RefreshManifestsAfterRootDeleteLocked_(touchedFolders, result);

    result.ok = (result.failedDeletes == 0);

    if (!result.ok && result.reason.empty()) {
        result.reason = L"一部セグメントの削除に失敗しました。";
    }

    return result;
}

std::uint64_t TimeShift_SegmentStore::CalcClosedBytesLocked_(
    const Session& s) const noexcept
{
    std::uint64_t total = 0;

    for (const auto& seg : s.segments) {
        if (seg.closed) {
            total += seg.bytes;
        }
    }

    return total;
}

void TimeShift_SegmentStore::SortSegmentsLocked_(Session& s) noexcept
{
    std::sort(
        s.segments.begin(),
        s.segments.end(),
        [](const SegmentInfo& a, const SegmentInfo& b)
        {
            if (a.sequence != b.sequence) {
                return a.sequence < b.sequence;
            }

            return a.filePath.filename().wstring() <
                b.filePath.filename().wstring();
        });
}

std::uint64_t TimeShift_SegmentStore::ParseSequenceFromSegmentName_(
    const std::filesystem::path& p) noexcept
{
    const std::wstring stem = p.stem().wstring();

    const std::size_t pos = stem.find_last_of(L'_');
    if (pos == std::wstring::npos || pos + 1 >= stem.size()) {
        return 0;
    }

    const std::wstring seqText = stem.substr(pos + 1);

    wchar_t* endPtr = nullptr;
    const unsigned long long value =
        std::wcstoull(seqText.c_str(), &endPtr, 10);

    if (endPtr == seqText.c_str()) {
        return 0;
    }

    return static_cast<std::uint64_t>(value);
}

bool TimeShift_SegmentStore::HasPrefix_(
    const std::wstring& s,
    const std::wstring& prefix) noexcept
{
    if (s.size() < prefix.size()) {
        return false;
    }

    return std::equal(prefix.begin(), prefix.end(), s.begin());
}

bool TimeShift_SegmentStore::ScanRootSegmentsForCleaner_(
    const Settings& settings,
    std::vector<RootSegmentInfo>& segments,
    QuotaStatus& result) const noexcept
{
    segments.clear();
    result.totalBefore = 0;

    std::error_code ec;

    if (!std::filesystem::exists(settings.rootFolder, ec)) {
        return true;
    }

    if (!std::filesystem::is_directory(settings.rootFolder, ec)) {
        result.reason = L"TimeShift rootFolder is not directory";
        return false;
    }

    ec.clear();

    std::filesystem::recursive_directory_iterator it(
        settings.rootFolder,
        std::filesystem::directory_options::skip_permission_denied,
        ec);

    const std::filesystem::recursive_directory_iterator end;

    while (!ec && it != end) {
        const std::filesystem::path p = it->path();

        bool isRegular = false;
        {
            std::error_code regEc;
            isRegular = it->is_regular_file(regEc);
        }

        if (isRegular && IsSegmentTsPath_(p)) {
            std::error_code sizeEc;
            const auto fileSize =
                std::filesystem::file_size(p, sizeEc);

            if (!sizeEc) {
                RootSegmentInfo seg{};
                seg.filePath = p;
                seg.channelFolder = p.parent_path();
                seg.startText = ParseStartTextFromSegmentName_(p);
                seg.sequence = ParseSequenceFromSegmentName_(p);
                seg.bytes = static_cast<std::uint64_t>(fileSize);

                segments.push_back(seg);
                result.totalBefore += seg.bytes;
            }
        }

        it.increment(ec);
    }

    if (ec) {
        result.reason = L"TimeShift rootFolder recursive scan failed";
        return false;
    }

    result.scannedSegments = segments.size();
    return true;
}

void TimeShift_SegmentStore::CleanupDeleteLeftoversForCleaner_(
    const Settings& settings,
    QuotaStatus& result) const noexcept
{
    std::error_code ec;

    if (!std::filesystem::exists(settings.rootFolder, ec)) {
        return;
    }

    ec.clear();

    std::filesystem::recursive_directory_iterator it(
        settings.rootFolder,
        std::filesystem::directory_options::skip_permission_denied,
        ec);

    const std::filesystem::recursive_directory_iterator end;

    while (!ec && it != end) {
        const std::filesystem::path p = it->path();

        bool isRegular = false;
        {
            std::error_code regEc;
            isRegular = it->is_regular_file(regEc);
        }

        if (isRegular && p.extension() == L".delete") {
            std::error_code rmEc;
            std::filesystem::remove(p, rmEc);
            if (!rmEc) {
                ++result.cleanupDeleteFiles;
            }
        }

        it.increment(ec);
    }
}

bool TimeShift_SegmentStore::DeleteRootSegmentForCleaner_(
    const RootSegmentInfo& seg,
    QuotaStatus& result) noexcept
{
    if (seg.filePath.empty()) {
        return false;
    }

    std::error_code ec;

    if (!std::filesystem::exists(seg.filePath, ec)) {
        return true;
    }

    // 念のため .tmp / .delete は触らない。
    if (seg.filePath.extension() != L".ts") {
        return false;
    }

    std::filesystem::path deletePath = seg.filePath;
    deletePath += L".delete";

    ec.clear();

    if (std::filesystem::exists(deletePath, ec)) {
        std::filesystem::remove(deletePath, ec);
        ec.clear();
    }

    // まず seg_*.ts から外す。
    // ViewIndex 側が seg_*.ts だけを見る前提なら、この時点で対象外になる。
    std::filesystem::rename(seg.filePath, deletePath, ec);
    if (ec) {
        ++result.failedDeletes;
        return false;
    }

    ++result.deletedSegments;
    result.deletedBytes += seg.bytes;

    ec.clear();
    std::filesystem::remove(deletePath, ec);
    if (ec) {
        // remove失敗時は .delete として残る。
        // 次回 CleanupDeleteLeftoversForCleaner_() で再削除する。
        ++result.failedDeletes;
    }

    return true;
}

bool TimeShift_SegmentStore::ScanRootSegmentsLocked_(
    std::vector<RootSegmentInfo>& segments,
    QuotaStatus& result) const noexcept
{
    segments.clear();
    result.totalBefore = 0;

    std::error_code ec;

    if (!std::filesystem::exists(settings_.rootFolder, ec)) {
        return true;
    }

    if (!std::filesystem::is_directory(settings_.rootFolder, ec)) {
        result.reason = L"TimeShift rootFolder is not directory";
        return false;
    }

    ec.clear();

    std::filesystem::recursive_directory_iterator it(
        settings_.rootFolder,
        std::filesystem::directory_options::skip_permission_denied,
        ec);

    const std::filesystem::recursive_directory_iterator end;

    while (!ec && it != end) {
        const std::filesystem::path p = it->path();

        bool isRegular = false;
        {
            std::error_code regEc;
            isRegular = it->is_regular_file(regEc);
        }

        if (isRegular && IsSegmentTsPath_(p)) {
            std::error_code sizeEc;
            const auto fileSize = std::filesystem::file_size(p, sizeEc);

            if (!sizeEc) {
                RootSegmentInfo seg{};
                seg.filePath = p;
                seg.channelFolder = p.parent_path();
                seg.startText = ParseStartTextFromSegmentName_(p);
                seg.sequence = ParseSequenceFromSegmentName_(p);
                seg.bytes = static_cast<std::uint64_t>(fileSize);

                segments.push_back(seg);
                result.totalBefore += seg.bytes;
            }
        }

        it.increment(ec);
    }

    if (ec) {
        result.reason = L"TimeShift rootFolder recursive scan failed";
        return false;
    }

    result.scannedSegments = segments.size();
    return true;
}

void TimeShift_SegmentStore::CleanupDeleteLeftoversLocked_(
    QuotaStatus& result) const noexcept
{
    std::error_code ec;

    if (!std::filesystem::exists(settings_.rootFolder, ec)) {
        return;
    }

    ec.clear();

    std::filesystem::recursive_directory_iterator it(
        settings_.rootFolder,
        std::filesystem::directory_options::skip_permission_denied,
        ec);

    const std::filesystem::recursive_directory_iterator end;

    while (!ec && it != end) {
        const std::filesystem::path p = it->path();

        bool isRegular = false;
        {
            std::error_code regEc;
            isRegular = it->is_regular_file(regEc);
        }

        if (isRegular && p.extension() == L".delete") {
            std::error_code rmEc;
            std::filesystem::remove(p, rmEc);
            if (!rmEc) {
                ++result.cleanupDeleteFiles;
            }
        }

        it.increment(ec);
    }
}

bool TimeShift_SegmentStore::DeleteRootSegmentLocked_(
    const RootSegmentInfo& seg,
    QuotaStatus& result) noexcept
{
    if (seg.filePath.empty()) {
        return false;
    }

    std::error_code ec;

    if (!std::filesystem::exists(seg.filePath, ec)) {
        return true;
    }

    std::filesystem::path deletePath = seg.filePath;
    deletePath += L".delete";

    ec.clear();

    if (std::filesystem::exists(deletePath, ec)) {
        std::filesystem::remove(deletePath, ec);
        ec.clear();
    }

    std::filesystem::rename(seg.filePath, deletePath, ec);
    if (ec) {
        ++result.failedDeletes;
        return false;
    }

    // この時点で seg_*.ts としては削除済み扱い。
    // remove に失敗しても .delete として次回起動時/次回監視で再削除する。
    ++result.deletedSegments;
    result.deletedBytes += seg.bytes;

    ec.clear();
    std::filesystem::remove(deletePath, ec);
    if (ec) {
        ++result.failedDeletes;
    }

    return true;
}

void TimeShift_SegmentStore::RefreshManifestsAfterRootDeleteLocked_(
    const std::vector<std::filesystem::path>& touchedFolders,
    QuotaStatus& result) noexcept
{
    for (const auto& folder : touchedFolders) {
        if (folder.empty()) {
            continue;
        }

        const std::wstring folderKey = MakePathKey_(folder);

        bool handledBySession = false;

        for (auto& pair : sessions_) {
            Session& s = pair.second;

            if (MakePathKey_(s.folder) != folderKey) {
                continue;
            }

            handledBySession = true;

            std::vector<SegmentInfo> kept;
            kept.reserve(s.segments.size());

            for (const auto& seg : s.segments) {
                if (seg.filePath.empty()) {
                    continue;
                }

                std::error_code ec;
                if (std::filesystem::exists(seg.filePath, ec)) {
                    kept.push_back(seg);
                }
            }

            s.segments.swap(kept);
            SortSegmentsLocked_(s);

            if (WriteManifestLocked_(s)) {
                ++result.manifestUpdated;
            }

            break;
        }

        if (!handledBySession) {
            if (RebuildManifestFromFolderLocked_(folder)) {
                ++result.manifestUpdated;
            }
        }
    }
}

bool TimeShift_SegmentStore::RebuildManifestFromFolderLocked_(
    const std::filesystem::path& folder) noexcept
{
    std::error_code ec;

    if (!std::filesystem::exists(folder, ec)) {
        return true;
    }

    if (!std::filesystem::is_directory(folder, ec)) {
        return false;
    }

    Session temp{};
    temp.folder = folder;

    ec.clear();

    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path p = it->path();

        bool isRegular = false;
        {
            std::error_code regEc;
            isRegular = it->is_regular_file(regEc);
        }

        if (!isRegular || !IsSegmentTsPath_(p)) {
            continue;
        }

        std::error_code sizeEc;
        const auto fileSize = std::filesystem::file_size(p, sizeEc);
        if (sizeEc) {
            continue;
        }

        SegmentInfo seg{};
        seg.sequence = ParseSequenceFromSegmentName_(p);
        seg.startText = ParseStartTextFromSegmentName_(p);
        seg.endText.clear();
        seg.filePath = p;
        seg.bytes = static_cast<std::uint64_t>(fileSize);
        seg.packetCount =
            (settings_.packetSize > 0)
            ? (seg.bytes / static_cast<std::uint64_t>(settings_.packetSize))
            : 0;
        seg.closed = true;

        temp.segments.push_back(std::move(seg));
    }

    SortSegmentsLocked_(temp);
    return WriteManifestLocked_(temp);
}

bool TimeShift_SegmentStore::IsSegmentTsPath_(
    const std::filesystem::path& p)
{
    if (p.extension() != L".ts") {
        return false;
    }

    const std::wstring name = p.filename().wstring();
    return HasPrefix_(name, L"seg_");
}

std::wstring TimeShift_SegmentStore::ParseStartTextFromSegmentName_(
    const std::filesystem::path& p)
{
    const std::wstring stem = p.stem().wstring();

    // seg_YYYYMMDD_HHMMSS_000001
    if (stem.size() >= 19 && HasPrefix_(stem, L"seg_")) {
        return stem.substr(4, 15);
    }

    return L"";
}

std::wstring TimeShift_SegmentStore::MakePathKey_(
    const std::filesystem::path& p)
{
    return p.lexically_normal().wstring();
}