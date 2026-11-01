#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RecordingEitAnalyzer.hpp"

#include "DebugManager.h"
#include "EITFilter.hpp"
#include "EITEventSortUnit.hpp"
#include "EitEventUpdateUnit.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <new>
#include <system_error>

extern bool WriteEitDumpTextFileFromParsed(
    const std::wstring& outPath,
    const std::vector<ParsedEITPacket>& events,
    int space,
    int ch,
    bool enableAribTrace);

struct RecordingEitAnalyzer::Session
{
    StartRequest req{};

    ULONGLONG startTickMs = 0;

    std::uint64_t inputPackets = 0;
    std::uint64_t eitPackets = 0;
    std::uint64_t parsedEventsLast = 0;

    bool dirty = false;

    EITFilter eitFilter;
    EitEventUpdateUnit updateStore;
};

RecordingEitAnalyzer::RecordingEitAnalyzer() = default;

RecordingEitAnalyzer::~RecordingEitAnalyzer() noexcept
{
    ResetAll();
}

namespace
{
    std::uint16_t GetPidFromTs_(const TsPacket& pkt) noexcept
    {
        const auto* p = pkt.data();

        if (p == nullptr || pkt.size() < 188) {
            return 0x1FFF;
        }

        if (p[0] != 0x47) {
            return 0x1FFF;
        }

        return static_cast<std::uint16_t>(
            ((static_cast<unsigned>(p[1]) & 0x1F) << 8) |
            static_cast<unsigned>(p[2]));
    }

    bool AppendFile_(
        const std::filesystem::path& src,
        const std::filesystem::path& dst)
    {
        std::ifstream in(src, std::ios::binary);
        if (!in) {
            return false;
        }

        std::ofstream out(dst, std::ios::binary | std::ios::app);
        if (!out) {
            return false;
        }

        out << in.rdbuf();
        return static_cast<bool>(out);
    }

    bool MoveOrReplace_(
        const std::filesystem::path& src,
        const std::filesystem::path& dst)
    {
        std::error_code ec{};

        std::filesystem::remove(dst, ec);
        ec.clear();

        std::filesystem::rename(src, dst, ec);
        if (!ec) {
            return true;
        }

        ec.clear();

        std::filesystem::copy_file(
            src,
            dst,
            std::filesystem::copy_options::overwrite_existing,
            ec);

        if (ec) {
            return false;
        }

        ec.clear();
        std::filesystem::remove(src, ec);
        return true;
    }

    bool ExistingEpgUsable_(
        const std::filesystem::path& path,
        std::uintmax_t minBytes)
    {
        std::error_code ec{};

        if (!std::filesystem::exists(path, ec) || ec) {
            return false;
        }

        ec.clear();
        const auto size = std::filesystem::file_size(path, ec);

        if (ec) {
            return false;
        }

        return size >= minBytes;
    }

    std::uintmax_t GetFileSizeOrZero_(
        const std::filesystem::path& path) noexcept
    {
        std::error_code ec{};

        if (!std::filesystem::exists(path, ec) || ec) {
            return 0;
        }

        ec.clear();
        const auto size = std::filesystem::file_size(path, ec);

        if (ec) {
            return 0;
        }

        return size;
    }
}

bool RecordingEitAnalyzer::StartSession(const StartRequest& req)
{
    if (req.instance == 0) {
        return false;
    }

    auto session = std::make_unique<Session>();
    session->req = req;
    session->startTickMs = ::GetTickCount64();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[req.instance] = std::move(session);
    }

    DebugLogFormat(
        L"[RecordingEitAnalyzer][START] inst=%u space=%d ch=%d nid=%u tsid=%u sid=%u maxSec=%u",
        static_cast<unsigned>(req.instance),
        req.space,
        req.physicalChannel,
        static_cast<unsigned>(req.networkId),
        static_cast<unsigned>(req.tsId),
        static_cast<unsigned>(req.serviceId),
        static_cast<unsigned>(req.maxAnalyzeSeconds));

    return true;
}

bool RecordingEitAnalyzer::UpdateResolvedChannel(
    std::uint32_t instance,
    int space,
    int physicalChannel) noexcept
{
    if (instance == 0 || physicalChannel < 0) {
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto it = sessions_.find(instance);
        if (it == sessions_.end() || !it->second) {
            return false;
        }

        Session& s = *it->second;
        s.req.space = space;
        s.req.physicalChannel = physicalChannel;

        DebugLogFormat(
            L"[RecordingEitAnalyzer][CHANNEL-RESOLVED] inst=%u space=%d ch=%d",
            static_cast<unsigned>(instance),
            space,
            physicalChannel);

        return true;
    }
    catch (...) {
        return false;
    }
}

RecordingEitAnalyzer::UpdateResult RecordingEitAnalyzer::UpdateFromPackets(
    std::uint32_t instance,
    const std::vector<TsPacket>& packets)
{
    UpdateResult r{};
    r.inputPackets = static_cast<std::uint64_t>(packets.size());

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = sessions_.find(instance);
    if (it == sessions_.end() || !it->second) {
        return r;
    }

    Session& s = *it->second;

    const ULONGLONG now = ::GetTickCount64();
    if (s.req.maxAnalyzeSeconds > 0) {
        const ULONGLONG limitMs =
            static_cast<ULONGLONG>(s.req.maxAnalyzeSeconds) * 1000ULL;

        if (now >= s.startTickMs && (now - s.startTickMs) > limitMs) {
            return r;
        }
    }

    std::array<std::uint8_t, 188> ts{};

    for (const auto& pkt : packets) {
        ++s.inputPackets;

        if (!IsEitPacket_(pkt)) {
            continue;
        }

        if (!CopyToTsArray_(pkt, ts)) {
            continue;
        }

        ++s.eitPackets;
        ++r.eitPackets;

        try {
            s.eitFilter.InputPacket(ts);
            s.dirty = true;
        }
        catch (...) {
            DebugLogFormat(
                L"[RecordingEitAnalyzer][WARN] EIT InputPacket exception inst=%u",
                static_cast<unsigned>(instance));
        }
    }

    const auto& events = s.eitFilter.GetEvents();
    r.parsedEvents = static_cast<std::uint64_t>(events.size());
    r.accepted = true;

    if (r.eitPackets > 0 &&
        (s.eitPackets <= 32 ||
            (s.eitPackets % 1000) == 0 ||
            r.parsedEvents != s.parsedEventsLast)) {
        s.parsedEventsLast = r.parsedEvents;

        DebugLogFormat(
            L"[RecordingEitAnalyzer][SEEN] inst=%u input=%llu eitPkt=%llu events=%llu",
            static_cast<unsigned>(instance),
            static_cast<unsigned long long>(s.inputPackets),
            static_cast<unsigned long long>(s.eitPackets),
            static_cast<unsigned long long>(r.parsedEvents));
    }

    return r;
}

RecordingEitAnalyzer::FlushResult RecordingEitAnalyzer::Flush(std::uint32_t instance)
{
    FlushResult r{};

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = sessions_.find(instance);
    if (it == sessions_.end() || !it->second) {
        r.reason = L"session not found";
        return r;
    }

    Session& s = *it->second;

    r.space = s.req.space;
    r.physicalChannel = s.req.physicalChannel;
    r.networkId = s.req.networkId;
    r.tsId = s.req.tsId;
    r.serviceId = s.req.serviceId;

    r.inputPackets = s.inputPackets;
    r.eitPackets = s.eitPackets;

    const auto outPath = MakeOutputPath_(s.req);
    r.outputPath = outPath.wstring();
    r.beforeSize = GetFileSizeOrZero_(outPath);

    if (!s.dirty) {
        r.ok = true;
        r.reason = L"not dirty";
        return r;
    }

    const auto& rawEvents = s.eitFilter.GetEvents();
    r.rawEvents = static_cast<std::uint64_t>(rawEvents.size());

    if (rawEvents.empty()) {
        r.ok = true;
        r.reason = L"no events";
        return r;
    }

    EitEventUpdateUnit::UpdateOptions upOpt{};
    upOpt.enablePreSort = true;
    upOpt.relaxKeyWhenInvalidStartTime = true;

    EitEventUpdateUnit::UpdateStats updateStats{};

    try {
        updateStats = s.updateStore.Apply(rawEvents, upOpt);
    }
    catch (const std::bad_alloc&) {
        r.reason = L"updateStore.Apply bad_alloc";
        return r;
    }
    catch (...) {
        r.reason = L"updateStore.Apply exception";
        return r;
    }

    r.updateAdded = updateStats.added;
    r.updateUpdated = updateStats.updated;
    r.updateReplaced = updateStats.replaced;
    r.updateUnchanged = updateStats.unchanged;
    r.updateFilledFields = updateStats.filledFields;
    r.updateMergedExtItems = updateStats.mergedExtItems;
    r.updateStoreSize = updateStats.storeSize;

    std::vector<ParsedEITPacket> snapshot;
    try {
        snapshot = s.updateStore.ExportSnapshotSorted();
    }
    catch (const std::bad_alloc&) {
        r.reason = L"ExportSnapshotSorted bad_alloc";
        return r;
    }
    catch (...) {
        r.reason = L"ExportSnapshotSorted exception";
        return r;
    }

    if (snapshot.empty()) {
        r.ok = true;
        r.reason = L"snapshot empty";
        return r;
    }

    std::vector<ParsedEITPacket> uniqueEvents;
    try {
        uniqueEvents = EITEventSortUnit::BuildUniqueSortedEvents(snapshot);
    }
    catch (const std::bad_alloc&) {
        r.reason = L"BuildUniqueSortedEvents bad_alloc";
        return r;
    }
    catch (...) {
        r.reason = L"BuildUniqueSortedEvents exception";
        return r;
    }

    r.uniqueEvents = static_cast<std::uint64_t>(uniqueEvents.size());
    if (r.rawEvents >= r.uniqueEvents) {
        r.duplicateInSession = r.rawEvents - r.uniqueEvents;
    }

    if (uniqueEvents.empty()) {
        r.ok = true;
        r.reason = L"unique empty";
        return r;
    }

    if (!s.req.enableWrite) {
        r.ok = true;
        r.wrote = false;
        r.reason = L"write disabled";
        return r;
    }

    std::error_code ec{};
    std::filesystem::create_directories(outPath.parent_path(), ec);

    const std::filesystem::path tmpPath =
        std::filesystem::path(outPath.wstring() + L".recording.tmp");

    ec.clear();
    std::filesystem::remove(tmpPath, ec);

    bool writeOk = false;

    try {
        writeOk = WriteEitDumpTextFileFromParsed(
            tmpPath.wstring(),
            uniqueEvents,
            s.req.space,
            s.req.physicalChannel,
            s.req.enableAribTrace);
    }
    catch (const std::bad_alloc&) {
        r.reason = L"WriteEitDumpTextFileFromParsed bad_alloc";
        return r;
    }
    catch (...) {
        r.reason = L"WriteEitDumpTextFileFromParsed exception";
        return r;
    }

    if (!writeOk) {
        r.reason = L"WriteEitDumpTextFileFromParsed failed";
        ec.clear();
        std::filesystem::remove(tmpPath, ec);
        return r;
    }

    r.tmpSize = GetFileSizeOrZero_(tmpPath);

    if (r.tmpSize == 0) {
        r.reason = L"tmp file missing or empty after write";

        ec.clear();
        std::filesystem::remove(tmpPath, ec);

        DebugLogFormat(
            L"[RecordingEitAnalyzer][ERROR] tmp missing or empty inst=%u tmp=%ls before=%llu raw=%llu unique=%llu",
            static_cast<unsigned>(instance),
            tmpPath.wstring().c_str(),
            static_cast<unsigned long long>(r.beforeSize),
            static_cast<unsigned long long>(r.rawEvents),
            static_cast<unsigned long long>(r.uniqueEvents));

        return r;
    }

    const bool append =
        ExistingEpgUsable_(outPath, s.req.existingEpgMinBytes);

    bool commitOk = false;

    if (append) {
        commitOk = AppendFile_(tmpPath, outPath);
        ec.clear();
        std::filesystem::remove(tmpPath, ec);
    }
    else {
        commitOk = MoveOrReplace_(tmpPath, outPath);
    }

    if (!commitOk) {
        r.reason = L"commit failed";
        return r;
    }

    r.afterSize = GetFileSizeOrZero_(outPath);

    if (r.afterSize == 0) {
        r.reason = L"output file missing or empty after commit";
        return r;
    }

    if (append) {
        r.sizeIncreased = r.afterSize > r.beforeSize;
        r.writtenBytes =
            r.sizeIncreased
            ? (r.afterSize - r.beforeSize)
            : 0;

        if (!r.sizeIncreased) {
            r.reason = L"append commit did not increase output file";

            DebugLogFormat(
                L"[RecordingEitAnalyzer][ERROR] append did not increase file inst=%u path=%ls before=%llu tmp=%llu after=%llu",
                static_cast<unsigned>(instance),
                outPath.wstring().c_str(),
                static_cast<unsigned long long>(r.beforeSize),
                static_cast<unsigned long long>(r.tmpSize),
                static_cast<unsigned long long>(r.afterSize));

            return r;
        }
    }
    else {
        r.sizeIncreased = r.afterSize > 0;
        r.writtenBytes = r.afterSize;
    }

    r.ok = true;
    r.wrote = true;
    r.appended = append;
    r.reason = append ? L"appended" : L"created";

    r.epgFileChanged =
        r.wrote &&
        r.sizeIncreased &&
        r.uniqueEvents > 0;

    r.taskCheckRecommended = r.epgFileChanged;

    s.dirty = false;

    DebugLogFormat(
        L"[RecordingEitAnalyzer][FLUSH] inst=%u ok=1 mode=%ls path=%ls raw=%llu unique=%llu dupSession=%llu add=%llu upd=%llu repl=%llu same=%llu filled=%llu mergedItems=%llu store=%llu input=%llu eitPkt=%llu before=%llu tmp=%llu after=%llu written=%llu increased=%d epgChanged=%d taskCheck=%d",
        static_cast<unsigned>(instance),
        append ? L"append" : L"create",
        outPath.wstring().c_str(),
        static_cast<unsigned long long>(r.rawEvents),
        static_cast<unsigned long long>(r.uniqueEvents),
        static_cast<unsigned long long>(r.duplicateInSession),
        static_cast<unsigned long long>(r.updateAdded),
        static_cast<unsigned long long>(r.updateUpdated),
        static_cast<unsigned long long>(r.updateReplaced),
        static_cast<unsigned long long>(r.updateUnchanged),
        static_cast<unsigned long long>(r.updateFilledFields),
        static_cast<unsigned long long>(r.updateMergedExtItems),
        static_cast<unsigned long long>(r.updateStoreSize),
        static_cast<unsigned long long>(r.inputPackets),
        static_cast<unsigned long long>(r.eitPackets),
        static_cast<unsigned long long>(r.beforeSize),
        static_cast<unsigned long long>(r.tmpSize),
        static_cast<unsigned long long>(r.afterSize),
        static_cast<unsigned long long>(r.writtenBytes),
        r.sizeIncreased ? 1 : 0,
        r.epgFileChanged ? 1 : 0,
        r.taskCheckRecommended ? 1 : 0);

    return r;
}

void RecordingEitAnalyzer::Reset(std::uint32_t instance) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(instance);
}

void RecordingEitAnalyzer::ResetAll() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
}

bool RecordingEitAnalyzer::IsActive(std::uint32_t instance) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.find(instance) != sessions_.end();
}

bool RecordingEitAnalyzer::IsEitPacket_(const TsPacket& pkt) noexcept
{
    return GetPidFromTs_(pkt) == 0x0012;
}

bool RecordingEitAnalyzer::CopyToTsArray_(
    const TsPacket& src,
    std::array<std::uint8_t, 188>& dst) noexcept
{
    if (src.data() == nullptr || src.size() < 188) {
        return false;
    }

    std::copy_n(src.data(), 188, dst.data());
    return dst[0] == 0x47;
}

std::filesystem::path RecordingEitAnalyzer::MakeOutputPath_(
    const StartRequest& req)
{
    std::filesystem::path dir =
        req.outputDir.empty()
        ? std::filesystem::path(L"epg_out")
        : std::filesystem::path(req.outputDir);

    std::wstring name;

    if (req.physicalChannel >= 0) {
        name =
            req.basePrefix +
            L"_space" +
            std::to_wstring(req.space) +
            L"_ch" +
            std::to_wstring(req.physicalChannel) +
            L"_EIT.epg";
    }
    else {
        name =
            L"EpgPiggy_space" +
            std::to_wstring(req.space) +
            L"_tsid" +
            std::to_wstring(req.tsId) +
            L"_sid" +
            std::to_wstring(req.serviceId) +
            L"_EIT.epg";
    }

    return dir / name;
}