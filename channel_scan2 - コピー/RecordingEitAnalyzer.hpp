#pragma once
#ifndef RECORDING_EIT_ANALYZER_HPP
#define RECORDING_EIT_ANALYZER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "TsStreamTypes.hpp"

class RecordingEitAnalyzer final
{
public:
    struct StartRequest
    {
        std::uint32_t instance = 0;

        int space = 0;
        int physicalChannel = -1;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;

        std::wstring outputDir = L"epg_out";
        std::wstring basePrefix = L"EpgFull";

        std::uint32_t maxAnalyzeSeconds = 180;

        std::uintmax_t existingEpgMinBytes = 4096;

        bool enableAribTrace = false;
        bool enableWrite = true;
    };

    struct UpdateResult
    {
        bool accepted = false;

        std::uint64_t inputPackets = 0;
        std::uint64_t eitPackets = 0;
        std::uint64_t parsedEvents = 0;
    };

    struct FlushResult
    {
        bool ok = false;
        bool wrote = false;
        bool appended = false;

        std::wstring outputPath;
        std::wstring reason;

        int space = 0;
        int physicalChannel = -1;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;

        std::uint64_t inputPackets = 0;
        std::uint64_t eitPackets = 0;
        std::uint64_t rawEvents = 0;
        std::uint64_t uniqueEvents = 0;

        std::uint64_t updateAdded = 0;
        std::uint64_t updateUpdated = 0;
        std::uint64_t updateReplaced = 0;
        std::uint64_t updateUnchanged = 0;
        std::uint64_t updateFilledFields = 0;
        std::uint64_t updateMergedExtItems = 0;
        std::uint64_t updateStoreSize = 0;

        std::uint64_t duplicateInSession = 0;

        std::uintmax_t beforeSize = 0;
        std::uintmax_t tmpSize = 0;
        std::uintmax_t afterSize = 0;
        std::uintmax_t writtenBytes = 0;
        bool sizeIncreased = false;

        bool epgFileChanged = false;
        bool taskCheckRecommended = false;
    };

public:
    RecordingEitAnalyzer();
    ~RecordingEitAnalyzer() noexcept;

    RecordingEitAnalyzer(const RecordingEitAnalyzer&) = delete;
    RecordingEitAnalyzer& operator=(const RecordingEitAnalyzer&) = delete;

    bool StartSession(const StartRequest& req);

    bool UpdateResolvedChannel(
        std::uint32_t instance,
        int space,
        int physicalChannel) noexcept;

    UpdateResult UpdateFromPackets(
        std::uint32_t instance,
        const std::vector<TsPacket>& packets);

    FlushResult Flush(std::uint32_t instance);

    void Reset(std::uint32_t instance) noexcept;
    void ResetAll() noexcept;

    bool IsActive(std::uint32_t instance) const;

private:
    struct Session;

private:
    static bool IsEitPacket_(const TsPacket& pkt) noexcept;

    static bool CopyToTsArray_(
        const TsPacket& src,
        std::array<std::uint8_t, 188>& dst) noexcept;

    static std::filesystem::path MakeOutputPath_(const StartRequest& req);

private:
    mutable std::mutex mutex_{};
    std::unordered_map<std::uint32_t, std::unique_ptr<Session>> sessions_{};
};

#endif // RECORDING_EIT_ANALYZER_HPP