#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Reservation_RawTsRecorder.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "IBonDriver2.h"
#include "TSStreamUtils.hpp"
#include "DebugManager.h"

namespace
{
    std::wstring GetLastErrorText_(DWORD err)
    {
        if (err == ERROR_SUCCESS) {
            return L"";
        }

        wchar_t* msg = nullptr;

        const DWORD n = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            err,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&msg),
            0,
            nullptr);

        std::wstring out;

        if (n != 0 && msg != nullptr) {
            out.assign(msg, msg + n);
            ::LocalFree(msg);

            while (!out.empty() &&
                (out.back() == L'\r' ||
                    out.back() == L'\n' ||
                    out.back() == L' ' ||
                    out.back() == L'\t')) {
                out.pop_back();
            }
        }
        else {
            out = L"Win32Error=" + std::to_wstring(static_cast<unsigned long>(err));
        }

        return out;
    }

    void CloseHandleSafe_(HANDLE& h) noexcept
    {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) {
            ::CloseHandle(h);
        }

        h = INVALID_HANDLE_VALUE;
    }

    bool EnsureParentDirectory_(
        const std::wstring& outputPath,
        std::wstring& reason)
    {
        reason.clear();

        try {
            const std::filesystem::path p(outputPath);
            const auto parent = p.parent_path();

            if (parent.empty()) {
                return true;
            }

            std::error_code ec;
            std::filesystem::create_directories(parent, ec);

            if (ec) {
                reason =
                    L"録画保存先フォルダの作成に失敗しました: " +
                    parent.wstring();
                return false;
            }

            return true;
        }
        catch (...) {
            reason = L"録画保存先フォルダの作成中に例外が発生しました。";
            return false;
        }
    }

    std::wstring Trim_(std::wstring s)
    {
        while (!s.empty() &&
            (s.front() == L' ' ||
                s.front() == L'\t' ||
                s.front() == L'\r' ||
                s.front() == L'\n')) {
            s.erase(s.begin());
        }

        while (!s.empty() &&
            (s.back() == L' ' ||
                s.back() == L'\t' ||
                s.back() == L'\r' ||
                s.back() == L'\n')) {
            s.pop_back();
        }

        return s;
    }

    bool ParseInt_(
        const std::wstring& s,
        int& out) noexcept
    {
        out = 0;

        try {
            std::size_t pos = 0;
            const std::wstring t = Trim_(s);

            if (t.empty()) {
                return false;
            }

            const int v = std::stoi(t, &pos, 10);
            out = v;
            return true;
        }
        catch (...) {
            return false;
        }
    }

    std::vector<std::wstring> SplitCsvSimple_(
        const std::wstring& line)
    {
        std::vector<std::wstring> cols;
        std::wstring cur;

        for (const wchar_t ch : line) {
            if (ch == L',') {
                cols.push_back(cur);
                cur.clear();
            }
            else {
                cur.push_back(ch);
            }
        }

        cols.push_back(cur);
        return cols;
    }

    std::filesystem::path GetExeDir_()
    {
        wchar_t exePath[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        std::filesystem::path base(exePath);
        return base.parent_path();
    }

    std::filesystem::path MakeDefaultConfigIniPath_()
    {
        return GetExeDir_() / L"config.ini";
    }

    std::filesystem::path MakeDefaultChannelListPath_(
        int space)
    {
        const std::wstring fileName =
            L"channellist_space" +
            std::to_wstring(space) +
            L".ch";

        return GetExeDir_() / fileName;
    }

    bool DecodeBytesToWide_(
        const std::vector<unsigned char>& bytes,
        std::wstring& out)
    {
        out.clear();

        if (bytes.empty()) {
            return true;
        }

        if (bytes.size() >= 2 &&
            bytes[0] == 0xFF &&
            bytes[1] == 0xFE) {
            const auto* p =
                reinterpret_cast<const wchar_t*>(bytes.data() + 2);

            const std::size_t wcharCount =
                (bytes.size() - 2) / sizeof(wchar_t);

            out.assign(p, p + wcharCount);
            return true;
        }

        const char* src = reinterpret_cast<const char*>(bytes.data());
        int srcLen = static_cast<int>(bytes.size());

        if (bytes.size() >= 3 &&
            bytes[0] == 0xEF &&
            bytes[1] == 0xBB &&
            bytes[2] == 0xBF) {
            src = reinterpret_cast<const char*>(bytes.data() + 3);
            srcLen = static_cast<int>(bytes.size() - 3);
        }

        int needed = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            src,
            srcLen,
            nullptr,
            0);

        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;

        if (needed <= 0) {
            codePage = CP_ACP;
            flags = 0;

            needed = ::MultiByteToWideChar(
                codePage,
                flags,
                src,
                srcLen,
                nullptr,
                0);
        }

        if (needed <= 0) {
            return false;
        }

        out.resize(static_cast<std::size_t>(needed));

        const int converted = ::MultiByteToWideChar(
            codePage,
            flags,
            src,
            srcLen,
            out.data(),
            needed);

        if (converted <= 0) {
            out.clear();
            return false;
        }

        return true;
    }

    bool LoadTextFile_(
        const std::filesystem::path& path,
        std::wstring& out,
        std::wstring& reason)
    {
        out.clear();
        reason.clear();

        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.wstring().c_str(), L"rb") != 0 || fp == nullptr) {
            reason = L"ファイルを開けません: " + path.wstring();
            return false;
        }

        std::vector<unsigned char> bytes;

        std::fseek(fp, 0, SEEK_END);
        const long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);

        if (size > 0) {
            bytes.resize(static_cast<std::size_t>(size));

            const std::size_t readSize =
                std::fread(bytes.data(), 1, bytes.size(), fp);

            if (readSize != bytes.size()) {
                std::fclose(fp);
                reason = L"ファイル読み込みサイズが一致しません: " + path.wstring();
                return false;
            }
        }

        std::fclose(fp);
        fp = nullptr;

        if (!DecodeBytesToWide_(bytes, out)) {
            reason = L"テキストデコードに失敗しました: " + path.wstring();
            return false;
        }

        return true;
    }

    struct ConfigIniChannelResolved_
    {
        bool ok = false;
        bool enabled = false;
        int space = 0;
        int channel = -1;
        std::wstring serviceName;
        std::wstring reason;
    };

    ConfigIniChannelResolved_ ResolveChannelFromConfigIni_(
        int targetSpace,
        std::uint16_t networkId,
        std::uint16_t tsId,
        std::uint16_t serviceId,
        const std::wstring& explicitConfigIniPath,
        bool allowDisabledService)
    {
        ConfigIniChannelResolved_ out{};
        out.space = targetSpace;

        std::filesystem::path iniPath;
        if (!explicitConfigIniPath.empty()) {
            iniPath = explicitConfigIniPath;
        }
        else {
            iniPath = MakeDefaultConfigIniPath_();
        }

        const std::wstring section =
            L"EpgServiceEnable_space" +
            std::to_wstring(targetSpace);

        const std::wstring key =
            std::to_wstring(static_cast<unsigned>(networkId)) +
            L"-" +
            std::to_wstring(static_cast<unsigned>(tsId)) +
            L"-" +
            std::to_wstring(static_cast<unsigned>(serviceId));

        const DWORD attr =
            ::GetFileAttributesW(iniPath.wstring().c_str());

        const bool fileExists =
            attr != INVALID_FILE_ATTRIBUTES &&
            (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;

        DebugLogFormat(
            L"[RawTsRecorder][CONFIG-LOOKUP] path=%ls exists=%d section=%ls key=%ls space=%d nid=%u tsid=%u sid=%u allowDisabled=%d",
            iniPath.wstring().c_str(),
            fileExists ? 1 : 0,
            section.c_str(),
            key.c_str(),
            targetSpace,
            static_cast<unsigned>(networkId),
            static_cast<unsigned>(tsId),
            static_cast<unsigned>(serviceId),
            allowDisabledService ? 1 : 0);

        wchar_t value[4096]{};

        const DWORD copied =
            ::GetPrivateProfileStringW(
                section.c_str(),
                key.c_str(),
                L"",
                value,
                static_cast<DWORD>(_countof(value)),
                iniPath.wstring().c_str());

        const std::wstring raw = Trim_(value);

        if (raw.empty()) {
            out.reason =
                L"config.ini に一致するサービスがありません。section=[" +
                section +
                L"] key=" +
                key +
                L" path=" +
                iniPath.wstring();

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=empty value copied=%lu exists=%d path=%ls section=%ls key=%ls",
                static_cast<unsigned long>(copied),
                fileExists ? 1 : 0,
                iniPath.wstring().c_str(),
                section.c_str(),
                key.c_str());

            return out;
        }

        DebugLogFormat(
            L"[RawTsRecorder][CONFIG-LOOKUP-HIT] copied=%lu raw=%ls",
            static_cast<unsigned long>(copied),
            raw.c_str());

        const auto cols = SplitCsvSimple_(raw);

        // value形式:
        // Enabled,ServiceName,Space,Channel,ServiceType,ServiceID,NetworkID,TSID,FreeCA,...
        if (cols.size() < 8) {
            out.reason =
                L"config.ini のサービス行の列数が不足しています。key=" +
                key;

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=too few columns key=%ls cols=%u raw=%ls",
                key.c_str(),
                static_cast<unsigned>(cols.size()),
                raw.c_str());

            return out;
        }

        int enabled = 0;
        int space = 0;
        int channel = -1;
        int sid = 0;
        int nid = 0;
        int tid = 0;

        if (!ParseInt_(cols[0], enabled)) {
            out.reason = L"config.ini の Enabled 値を解析できません。key=" + key;

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=parse Enabled key=%ls value=%ls raw=%ls",
                key.c_str(),
                cols[0].c_str(),
                raw.c_str());

            return out;
        }

        if (!ParseInt_(cols[2], space)) {
            out.reason = L"config.ini の Space 値を解析できません。key=" + key;

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=parse Space key=%ls value=%ls raw=%ls",
                key.c_str(),
                cols[2].c_str(),
                raw.c_str());

            return out;
        }

        if (!ParseInt_(cols[3], channel)) {
            out.reason = L"config.ini の Channel 値を解析できません。key=" + key;

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=parse Channel key=%ls value=%ls raw=%ls",
                key.c_str(),
                cols[3].c_str(),
                raw.c_str());

            return out;
        }

        if (!ParseInt_(cols[5], sid)) {
            out.reason = L"config.ini の ServiceID 値を解析できません。key=" + key;

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=parse ServiceID key=%ls value=%ls raw=%ls",
                key.c_str(),
                cols[5].c_str(),
                raw.c_str());

            return out;
        }

        if (!ParseInt_(cols[6], nid)) {
            out.reason = L"config.ini の NetworkID 値を解析できません。key=" + key;

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=parse NetworkID key=%ls value=%ls raw=%ls",
                key.c_str(),
                cols[6].c_str(),
                raw.c_str());

            return out;
        }

        if (!ParseInt_(cols[7], tid)) {
            out.reason = L"config.ini の TSID 値を解析できません。key=" + key;

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=parse TSID key=%ls value=%ls raw=%ls",
                key.c_str(),
                cols[7].c_str(),
                raw.c_str());

            return out;
        }

        if (space != targetSpace ||
            nid != static_cast<int>(networkId) ||
            tid != static_cast<int>(tsId) ||
            sid != static_cast<int>(serviceId)) {
            out.reason =
                L"config.ini のサービス行が予約IDと一致しません。key=" +
                key;

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=id mismatch key=%ls req(space=%d nid=%u tsid=%u sid=%u) ini(space=%d nid=%d tsid=%d sid=%d) raw=%ls",
                key.c_str(),
                targetSpace,
                static_cast<unsigned>(networkId),
                static_cast<unsigned>(tsId),
                static_cast<unsigned>(serviceId),
                space,
                nid,
                tid,
                sid,
                raw.c_str());

            return out;
        }

        if (enabled == 0 && !allowDisabledService) {
            out.reason =
                L"config.ini 上でサービスが無効です。key=" +
                key;

            DebugLogFormat(
                L"[RawTsRecorder][CONFIG-LOOKUP-FAIL] reason=disabled service key=%ls service=%ls ch=%d raw=%ls",
                key.c_str(),
                cols[1].c_str(),
                channel,
                raw.c_str());

            return out;
        }

        out.ok = true;
        out.enabled = (enabled != 0);
        out.space = space;
        out.channel = channel;
        out.serviceName = cols[1];

        DebugLogFormat(
            L"[RawTsRecorder][CHANNEL-RESOLVE-CONFIG] space=%d ch=%d enabled=%d nid=%u tsid=%u sid=%u service=%ls path=%ls",
            space,
            channel,
            out.enabled ? 1 : 0,
            static_cast<unsigned>(networkId),
            static_cast<unsigned>(tsId),
            static_cast<unsigned>(serviceId),
            out.serviceName.c_str(),
            iniPath.wstring().c_str());

        return out;
    }

    struct ChannelListResolved_
    {
        bool ok = false;
        int space = 0;
        int channel = -1;
        std::wstring serviceName;
        std::wstring reason;
    };

    ChannelListResolved_ ResolveChannelFromChannelList_(
        int targetSpace,
        std::uint16_t networkId,
        std::uint16_t tsId,
        std::uint16_t serviceId,
        const std::wstring& explicitPath)
    {
        ChannelListResolved_ out{};
        out.space = targetSpace;

        std::filesystem::path path;
        if (!explicitPath.empty()) {
            path = explicitPath;
        }
        else {
            path = MakeDefaultChannelListPath_(targetSpace);
        }

        std::wstring text;
        std::wstring reason;

        if (!LoadTextFile_(path, text, reason)) {
            out.reason = reason;
            return out;
        }

        std::size_t pos = 0;
        int lineNo = 0;

        while (pos < text.size()) {
            std::size_t end = text.find(L'\n', pos);
            if (end == std::wstring::npos) {
                end = text.size();
            }

            ++lineNo;

            std::wstring line =
                Trim_(text.substr(pos, end - pos));

            pos = end + 1;

            if (line.empty()) {
                continue;
            }

            if (line.front() == L';' || line.front() == L'#') {
                continue;
            }

            const auto cols = SplitCsvSimple_(line);

            // ServiceName,Space,Channel,ServiceType,ServiceID,NetworkID,TSID,ONID,...
            if (cols.size() < 7) {
                continue;
            }

            int space = 0;
            int channel = -1;
            int sid = 0;
            int nid = 0;
            int tid = 0;

            if (!ParseInt_(cols[1], space)) {
                continue;
            }
            if (!ParseInt_(cols[2], channel)) {
                continue;
            }
            if (!ParseInt_(cols[4], sid)) {
                continue;
            }
            if (!ParseInt_(cols[5], nid)) {
                continue;
            }
            if (!ParseInt_(cols[6], tid)) {
                continue;
            }

            if (space != targetSpace) {
                continue;
            }

            if (nid == static_cast<int>(networkId) &&
                tid == static_cast<int>(tsId) &&
                sid == static_cast<int>(serviceId)) {
                out.ok = true;
                out.space = space;
                out.channel = channel;
                out.serviceName = cols[0];

                DebugLogFormat(
                    L"[RawTsRecorder][CHANNEL-RESOLVE-CHLIST] line=%d space=%d ch=%d nid=%d tsid=%d sid=%d service=%ls",
                    lineNo,
                    space,
                    channel,
                    nid,
                    tid,
                    sid,
                    out.serviceName.c_str());

                return out;
            }
        }

        out.reason =
            L"channellist に一致するサービスがありません。path=" +
            path.wstring() +
            L" space=" +
            std::to_wstring(targetSpace) +
            L" nid=" +
            std::to_wstring(static_cast<unsigned>(networkId)) +
            L" tsid=" +
            std::to_wstring(static_cast<unsigned>(tsId)) +
            L" sid=" +
            std::to_wstring(static_cast<unsigned>(serviceId));

        return out;
    }

    bool WriteExact_(
        HANDLE file,
        const void* data,
        std::size_t size,
        std::wstring& reason)
    {
        reason.clear();

        if (file == INVALID_HANDLE_VALUE || file == nullptr) {
            reason = L"出力ファイルハンドルが無効です。";
            return false;
        }

        if (data == nullptr || size == 0) {
            reason = L"書き込みデータが空です。";
            return false;
        }

        const BYTE* p = static_cast<const BYTE*>(data);
        std::size_t remain = size;

        while (remain > 0) {
            const DWORD chunk =
                static_cast<DWORD>(
                    (std::min<std::size_t>)(
                        remain,
                        static_cast<std::size_t>(4u * 1024u * 1024u)));

            DWORD written = 0;

            const BOOL ok = ::WriteFile(
                file,
                p,
                chunk,
                &written,
                nullptr);

            if (!ok || written == 0) {
                const DWORD err = ::GetLastError();
                reason = L"WriteFile に失敗しました: " + GetLastErrorText_(err);
                return false;
            }

            if (written > chunk) {
                reason = L"WriteFile の書き込みサイズが異常です。";
                return false;
            }

            p += written;
            remain -= written;
        }

        return true;
    }

    bool WritePackets_(
        HANDLE file,
        const std::vector<TsPacket>& packets,
        std::uint64_t& outBytes,
        std::uint64_t& outPackets,
        std::wstring& reason)
    {
        outBytes = 0;
        outPackets = 0;
        reason.clear();

        if (packets.empty()) {
            reason = L"TSパケットが空です。";
            return false;
        }

        if constexpr (sizeof(TsPacket) == TS_PACKET_SIZE) {
            const std::size_t bytes =
                packets.size() * static_cast<std::size_t>(TS_PACKET_SIZE);

            if (!WriteExact_(file, packets.data(), bytes, reason)) {
                return false;
            }

            outBytes = static_cast<std::uint64_t>(bytes);
            outPackets = static_cast<std::uint64_t>(packets.size());
            return true;
        }
        else {
            for (const auto& pkt : packets) {
                const auto* p = pkt.data();
                const std::size_t bytes = pkt.size();

                if (p == nullptr || bytes != TS_PACKET_SIZE) {
                    reason = L"TsPacket のサイズが188byteではありません。";
                    return false;
                }

                if (!WriteExact_(file, p, bytes, reason)) {
                    return false;
                }

                outBytes += static_cast<std::uint64_t>(bytes);
                ++outPackets;
            }

            return true;
        }
    }

    void DrainInitialBlocks_(
        IBonDriver2* bon,
        int discardBlocks,
        std::uint64_t& outBlocks,
        std::uint64_t& outPackets)
    {
        outBlocks = 0;
        outPackets = 0;

        if (bon == nullptr || discardBlocks <= 0) {
            return;
        }

        std::vector<TsPacket> packets;
        packets.reserve(4096);

        for (int i = 0; i < discardBlocks; ++i) {
            packets.clear();

            if (TSStreamUtils::ReceiveTsStreamBlock(bon, packets)) {
                ++outBlocks;
                outPackets += static_cast<std::uint64_t>(packets.size());
            }

            ::Sleep(10);
        }
    }

    std::wstring GetExeDirectoryConfigIniPath_()
    {
        wchar_t exePath[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        std::filesystem::path p(exePath);
        p = p.parent_path() / L"config.ini";

        return p.wstring();
    }
}

Reservation_RawTsRecorder::~Reservation_RawTsRecorder() noexcept
{
    ResetAll();
}

bool Reservation_RawTsRecorder::StartTsCapture(
    const TsCaptureRequest& req,
    Result* out)
{
    StartRequest sr{};

    sr.instance = req.instance;
    sr.bon = req.bon;
    sr.outputPath = req.outputPath;

    sr.purpose = StartPurpose::TsCapture;

    sr.setChannelBeforeStart = req.setChannelBeforeStart;
    sr.space = req.space;
    sr.channel = req.channel;

    // TS機能用では、予約サービスIDからのチャンネル解決を使わない。
    // 必要なら caller 側で space/channel を指定する。
    sr.resolveChannelFromConfigIni = false;
    sr.configIniPath.clear();
    sr.allowDisabledServiceForChannelResolve = true;

    sr.resolveChannelFromChannelList = false;
    sr.channelListPath.clear();

    sr.networkId = 0;
    sr.tsId = 0;
    sr.serviceId = 0;
    sr.serviceName.clear();

    sr.waitForSignalLock = req.waitForSignalLock;
    sr.signalLockTimeoutMs = req.signalLockTimeoutMs;

    sr.settleSleepMs = req.settleSleepMs;
    sr.discardInitialBlocks = req.discardInitialBlocks;

    sr.idleSleepMs = req.idleSleepMs;
    sr.noDataTimeoutMs = req.noDataTimeoutMs;
    sr.flushIntervalBytes = req.flushIntervalBytes;
    sr.createParentDirectories = req.createParentDirectories;

    sr.onPackets = req.onPackets;

    return Start(sr, out);
}

bool Reservation_RawTsRecorder::Start(
    const StartRequest& req,
    Result* out)
{
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

    if (out) {
        *out = Result{};
        out->outputPath = req.outputPath;
    }

    if (req.instance == 0) {
        if (out) {
            out->ok = false;
            out->reason = L"instance が 0 です。";
        }
        return false;
    }

    if (req.bon == nullptr) {
        if (out) {
            out->ok = false;
            out->reason = L"IBonDriver2 が nullptr です。";
        }
        return false;
    }

    if (req.outputPath.empty()) {
        if (out) {
            out->ok = false;
            out->reason = L"録画保存先が空です。";
        }
        return false;
    }

    {
#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
        std::scoped_lock mapGuard(mutex_);

        if (sessions_.find(req.instance) != sessions_.end()) {
            if (out) {
                out->ok = false;
                out->reason = L"同じ instance の内部録画が既に実行中です。";
            }
            return false;
        }
    }

    bool channelSetTried = false;
    bool channelSetOk = false;

    bool channelResolved = false;
    bool resolvedFromConfigIni = false;
    bool resolvedServiceEnabled = false;

    int channelToSet = req.channel;
    std::wstring resolveReason;

    if (req.setChannelBeforeStart &&
        channelToSet < 0 &&
        req.resolveChannelFromConfigIni) {
        const auto resolved =
            ResolveChannelFromConfigIni_(
                req.space,
                req.networkId,
                req.tsId,
                req.serviceId,
                req.configIniPath,
                req.allowDisabledServiceForChannelResolve);

        if (resolved.ok) {
            channelToSet = resolved.channel;
            channelResolved = true;
            resolvedFromConfigIni = true;
            resolvedServiceEnabled = resolved.enabled;
        }
        else {
            resolveReason = resolved.reason;
        }
    }

    if (req.setChannelBeforeStart &&
        channelToSet < 0 &&
        req.resolveChannelFromChannelList) {
        DebugLogFormat(
            L"[RawTsRecorder][CHANNEL-RESOLVE-FALLBACK] config failed, try channellist inst=%u space=%d nid=%u tsid=%u sid=%u reason=%ls",
            static_cast<unsigned>(req.instance),
            req.space,
            static_cast<unsigned>(req.networkId),
            static_cast<unsigned>(req.tsId),
            static_cast<unsigned>(req.serviceId),
            resolveReason.c_str());

        const auto resolved =
            ResolveChannelFromChannelList_(
                req.space,
                req.networkId,
                req.tsId,
                req.serviceId,
                req.channelListPath);

        if (resolved.ok) {
            channelToSet = resolved.channel;
            channelResolved = true;
            resolvedFromConfigIni = false;
            resolvedServiceEnabled = true;
        }
        else {
            if (!resolveReason.empty()) {
                resolveReason += L" / ";
            }
            resolveReason += resolved.reason;
        }
    }

    if (req.setChannelBeforeStart && channelToSet < 0) {
        if (out) {
            out->ok = false;
            out->channelResolved = false;
            out->resolvedChannel = -1;
            out->resolvedFromConfigIni = false;
            out->resolvedServiceEnabled = false;
            out->reason =
                resolveReason.empty()
                ? L"録画開始前チャンネル指定が無効です。"
                : resolveReason;
        }

        DebugLogFormat(
            L"[RawTsRecorder][CHANNEL-RESOLVE-FAIL] inst=%u nid=%u tsid=%u sid=%u reason=%ls",
            static_cast<unsigned>(req.instance),
            static_cast<unsigned>(req.networkId),
            static_cast<unsigned>(req.tsId),
            static_cast<unsigned>(req.serviceId),
            resolveReason.c_str());

        return false;
    }

    if (req.setChannelBeforeStart) {
        channelSetTried = true;

        channelSetOk =
            req.bon->SetChannel(
                static_cast<DWORD>((std::max)(0, req.space)),
                static_cast<DWORD>(channelToSet)) != FALSE;

        if (!channelSetOk) {
            if (out) {
                out->ok = false;
                out->channelSetTried = true;
                out->channelSetOk = false;
                out->channelResolved = channelResolved;
                out->resolvedChannel = channelToSet;
                out->resolvedFromConfigIni = resolvedFromConfigIni;
                out->resolvedServiceEnabled = resolvedServiceEnabled;
                out->reason =
                    L"録画開始前の SetChannel に失敗しました。space=" +
                    std::to_wstring(req.space) +
                    L" channel=" +
                    std::to_wstring(channelToSet);
            }

            DebugLogFormat(
                L"[RawTsRecorder][START-FAIL] SetChannel failed inst=%u space=%d ch=%d resolved=%d fromConfig=%d nid=%u tsid=%u sid=%u",
                static_cast<unsigned>(req.instance),
                req.space,
                channelToSet,
                channelResolved ? 1 : 0,
                resolvedFromConfigIni ? 1 : 0,
                static_cast<unsigned>(req.networkId),
                static_cast<unsigned>(req.tsId),
                static_cast<unsigned>(req.serviceId));

            return false;
        }

        if (req.settleSleepMs > 0) {
            ::Sleep(req.settleSleepMs);
        }
    }

    bool signalLockTried = false;
    bool signalLockOk = false;

    if (req.waitForSignalLock) {
        signalLockTried = true;

        const int timeoutMs =
            (std::max)(500, req.signalLockTimeoutMs);

        signalLockOk =
            TSStreamUtils::WaitForSignalLock(req.bon, timeoutMs);

        if (!signalLockOk) {
            if (out) {
                out->ok = false;
                out->channelSetTried = channelSetTried;
                out->channelSetOk = channelSetOk;
                out->channelResolved = channelResolved;
                out->resolvedChannel = channelToSet;
                out->resolvedFromConfigIni = resolvedFromConfigIni;
                out->resolvedServiceEnabled = resolvedServiceEnabled;
                out->signalLockTried = true;
                out->signalLockOk = false;
                out->reason = L"録画開始前の信号ロックに失敗しました。";
            }

            DebugLogFormat(
                L"[RawTsRecorder][START-FAIL] signal lock failed inst=%u setCh=%d setChOk=%d resolved=%d fromConfig=%d space=%d ch=%d nid=%u tsid=%u sid=%u",
                static_cast<unsigned>(req.instance),
                channelSetTried ? 1 : 0,
                channelSetOk ? 1 : 0,
                channelResolved ? 1 : 0,
                resolvedFromConfigIni ? 1 : 0,
                req.space,
                channelToSet,
                static_cast<unsigned>(req.networkId),
                static_cast<unsigned>(req.tsId),
                static_cast<unsigned>(req.serviceId));

            return false;
        }
    }

    if (req.createParentDirectories) {
        std::wstring reason;
        if (!EnsureParentDirectory_(req.outputPath, reason)) {
            if (out) {
                out->ok = false;
                out->channelSetTried = channelSetTried;
                out->channelSetOk = channelSetOk;
                out->channelResolved = channelResolved;
                out->resolvedChannel = channelToSet;
                out->resolvedFromConfigIni = resolvedFromConfigIni;
                out->resolvedServiceEnabled = resolvedServiceEnabled;
                out->signalLockTried = signalLockTried;
                out->signalLockOk = signalLockOk;
                out->reason = reason;
            }

            return false;
        }
    }

    HANDLE file = ::CreateFileW(
        req.outputPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();

        if (out) {
            out->ok = false;
            out->channelSetTried = channelSetTried;
            out->channelSetOk = channelSetOk;
            out->channelResolved = channelResolved;
            out->resolvedChannel = channelToSet;
            out->resolvedFromConfigIni = resolvedFromConfigIni;
            out->resolvedServiceEnabled = resolvedServiceEnabled;
            out->signalLockTried = signalLockTried;
            out->signalLockOk = signalLockOk;
            out->reason =
                L"録画ファイルの作成に失敗しました: " +
                GetLastErrorText_(err);
        }

        return false;
    }

    std::uint64_t discardedBlocks = 0;
    std::uint64_t discardedPackets = 0;

    if (req.discardInitialBlocks > 0) {
        DrainInitialBlocks_(
            req.bon,
            req.discardInitialBlocks,
            discardedBlocks,
            discardedPackets);
    }

    auto session = std::make_shared<Session>();

    session->instance = req.instance;
    session->bon = req.bon;
    session->outputPath = req.outputPath;

    session->purpose = req.purpose;
    session->onPackets = req.onPackets;

    session->idleSleepMs =
        (std::max<DWORD>)(1, req.idleSleepMs);

    session->noDataTimeoutMs = req.noDataTimeoutMs;

    session->flushIntervalBytes =
        (std::max<std::uint64_t>)(
            1024ull * 1024ull,
            req.flushIntervalBytes);

    session->startTickMs = ::GetTickCount64();
    session->lastDataTickMs = session->startTickMs;
    session->file = file;

    {
#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
        std::scoped_lock statGuard(session->statMutex);

        session->stat.ok = true;
        session->stat.outputPath = req.outputPath;

        session->stat.channelResolved = channelResolved;
        session->stat.resolvedChannel = channelToSet;
        session->stat.resolvedFromConfigIni = resolvedFromConfigIni;
        session->stat.resolvedServiceEnabled = resolvedServiceEnabled;

        session->stat.channelSetTried = channelSetTried;
        session->stat.channelSetOk = channelSetOk;

        session->stat.signalLockTried = signalLockTried;
        session->stat.signalLockOk = signalLockOk;

        session->stat.discardedBlocks = discardedBlocks;
        session->stat.discardedPackets = discardedPackets;
    }

    {
#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
        std::scoped_lock mapGuard(mutex_);
        sessions_[req.instance] = session;
    }

    try {
        session->worker =
            std::thread(
                &Reservation_RawTsRecorder::Worker_,
                session);
    }
    catch (...) {
        {
#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
            std::scoped_lock mapGuard(mutex_);
            sessions_.erase(req.instance);
        }

        CloseHandleSafe_(session->file);

        if (out) {
            out->ok = false;
            out->channelResolved = channelResolved;
            out->resolvedChannel = channelToSet;
            out->resolvedFromConfigIni = resolvedFromConfigIni;
            out->resolvedServiceEnabled = resolvedServiceEnabled;
            out->channelSetTried = channelSetTried;
            out->channelSetOk = channelSetOk;
            out->signalLockTried = signalLockTried;
            out->signalLockOk = signalLockOk;
            out->reason = L"録画ワーカースレッドの起動に失敗しました。";
        }

        return false;
    }

    if (out) {
        *out = Snapshot_(session);
    }

    DebugLogFormat(
        L"[RawTsRecorder][START] instance=%u purpose=%d path=%ls setCh=%d setChOk=%d resolved=%d fromConfig=%d serviceEnabled=%d ch=%d sig=%d sigOk=%d discardBlocks=%llu discardPackets=%llu",
        static_cast<unsigned>(req.instance),
        static_cast<int>(req.purpose),
        req.outputPath.c_str(),
        channelSetTried ? 1 : 0,
        channelSetOk ? 1 : 0,
        channelResolved ? 1 : 0,
        resolvedFromConfigIni ? 1 : 0,
        resolvedServiceEnabled ? 1 : 0,
        channelToSet,
        signalLockTried ? 1 : 0,
        signalLockOk ? 1 : 0,
        static_cast<unsigned long long>(discardedBlocks),
        static_cast<unsigned long long>(discardedPackets));

    return true;
}

Reservation_RawTsRecorder::Result Reservation_RawTsRecorder::Stop(
    std::uint32_t instance)
{
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

    std::shared_ptr<Session> session;

    {
#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
        std::scoped_lock mapGuard(mutex_);

        const auto it = sessions_.find(instance);
        if (it == sessions_.end()) {
            Result r{};
            r.ok = false;
            r.reason = L"指定 instance の内部録画セッションがありません。";
            return r;
        }

        session = it->second;
        sessions_.erase(it);
    }

    session->stopRequested.store(true, std::memory_order_release);

    if (session->worker.joinable()) {
        try {
            session->worker.join();
        }
        catch (...) {
            MarkFailure_(
                session,
                L"録画ワーカースレッドの join に失敗しました。");
        }
    }

    if (session->file != INVALID_HANDLE_VALUE &&
        session->file != nullptr) {
        ::FlushFileBuffers(session->file);
        CloseHandleSafe_(session->file);
    }

    {
#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
        std::scoped_lock statGuard(session->statMutex);

        if (session->stat.bytesWritten == 0) {
            session->stat.ok = false;

            if (session->stat.reason.empty()) {
                session->stat.reason =
                    L"録画ファイルにTSデータを書き込めませんでした。";
            }
        }
    }

    Result r = Snapshot_(session);

    DebugLogFormat(
        L"[RawTsRecorder][STOP] instance=%u ok=%d bytes=%llu packets=%llu blocks=%llu recvFail=%llu writeFail=%llu callbackFail=%llu discardBlocks=%llu discardPackets=%llu path=%ls reason=%ls",
        static_cast<unsigned>(instance),
        r.ok ? 1 : 0,
        static_cast<unsigned long long>(r.bytesWritten),
        static_cast<unsigned long long>(r.packetsWritten),
        static_cast<unsigned long long>(r.blocksWritten),
        static_cast<unsigned long long>(r.receiveFailures),
        static_cast<unsigned long long>(r.writeFailures),
        static_cast<unsigned long long>(r.callbackFailures),
        static_cast<unsigned long long>(r.discardedBlocks),
        static_cast<unsigned long long>(r.discardedPackets),
        r.outputPath.c_str(),
        r.reason.c_str());

    return r;
}

void Reservation_RawTsRecorder::Reset(
    std::uint32_t instance) noexcept
{
    try {
        (void)Stop(instance);
    }
    catch (...) {
    }
}

void Reservation_RawTsRecorder::ResetAll() noexcept
{
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

    std::unordered_map<std::uint32_t, std::shared_ptr<Session>> sessions;

    {
#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
        std::scoped_lock mapGuard(mutex_);
        sessions.swap(sessions_);
    }

    for (auto& kv : sessions) {
        const auto& session = kv.second;
        if (!session) {
            continue;
        }

        session->stopRequested.store(true, std::memory_order_release);

        if (session->worker.joinable()) {
            try {
                session->worker.join();
            }
            catch (...) {
            }
        }

        if (session->file != INVALID_HANDLE_VALUE &&
            session->file != nullptr) {
            ::FlushFileBuffers(session->file);
            CloseHandleSafe_(session->file);
        }
    }
}

bool Reservation_RawTsRecorder::IsRunning(
    std::uint32_t instance) const
{
#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
    std::scoped_lock mapGuard(mutex_);

    return sessions_.find(instance) != sessions_.end();
}

void Reservation_RawTsRecorder::Worker_(
    const std::shared_ptr<Session>& session) noexcept
{
    if (!session) {
        return;
    }

    std::vector<TsPacket> packets;
    packets.reserve(4096);

    while (!session->stopRequested.load(std::memory_order_acquire)) {
        packets.clear();

        const bool received =
            TSStreamUtils::ReceiveTsStreamBlock(
                session->bon,
                packets);

        if (!received || packets.empty()) {
            AddReceiveFailure_(session);

            if (session->noDataTimeoutMs > 0) {
                const ULONGLONG now = ::GetTickCount64();
                const ULONGLONG elapsedNoData =
                    now - session->lastDataTickMs;

                if (elapsedNoData >=
                    static_cast<ULONGLONG>(session->noDataTimeoutMs)) {
                    MarkNoDataTimeout_(
                        session,
                        L"録画開始後、一定時間TSデータを受信できませんでした。");

                    session->stopRequested.store(true, std::memory_order_release);
                    break;
                }
            }

            ::Sleep(session->idleSleepMs);
            continue;
        }

        session->lastDataTickMs = ::GetTickCount64();

        std::uint64_t bytes = 0;
        std::uint64_t packetCount = 0;
        std::wstring reason;

        const bool writeOk =
            WritePackets_(
                session->file,
                packets,
                bytes,
                packetCount,
                reason);

        if (!writeOk) {
            MarkFailure_(
                session,
                reason.empty()
                ? L"TSパケットの書き込みに失敗しました。"
                : reason);

            session->stopRequested.store(true, std::memory_order_release);
            break;
        }

        AddWriteStats_(
            session,
            bytes,
            packetCount);

        std::uint64_t blockIndex = 0;
        std::uint64_t totalPackets = 0;
        std::uint64_t totalBytes = 0;

        {
#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
            std::scoped_lock statGuard(session->statMutex);

            blockIndex = session->stat.blocksWritten;
            totalPackets = session->stat.packetsWritten;
            totalBytes = session->stat.bytesWritten;
        }

        if (session->onPackets) {
            try {
                PacketBlock block{};
                block.purpose = session->purpose;
                block.instance = session->instance;
                block.blockIndex = blockIndex;
                block.totalPackets = totalPackets;
                block.totalBytes = totalBytes;
                block.packets = &packets;

                session->onPackets(block);
            }
            catch (...) {
                AddCallbackFailure_(session);
            }
        }

        session->bytesSinceFlush += bytes;

        if (session->bytesSinceFlush >= session->flushIntervalBytes) {
            ::FlushFileBuffers(session->file);
            session->bytesSinceFlush = 0;
        }
    }

    if (session->file != INVALID_HANDLE_VALUE &&
        session->file != nullptr) {
        ::FlushFileBuffers(session->file);
    }
}

Reservation_RawTsRecorder::Result Reservation_RawTsRecorder::Snapshot_(
    const std::shared_ptr<Session>& session) noexcept
{
    if (!session) {
        Result r{};
        r.ok = false;
        r.reason = L"session が nullptr です。";
        return r;
    }

#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
    std::scoped_lock statGuard(session->statMutex);

    return session->stat;
}

void Reservation_RawTsRecorder::AddReceiveFailure_(
    const std::shared_ptr<Session>& session) noexcept
{
    if (!session) {
        return;
    }

#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
    std::scoped_lock statGuard(session->statMutex);

    ++session->stat.receiveFailures;
}

void Reservation_RawTsRecorder::AddWriteStats_(
    const std::shared_ptr<Session>& session,
    std::uint64_t bytes,
    std::uint64_t packets) noexcept
{
    if (!session) {
        return;
    }

#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
    std::scoped_lock statGuard(session->statMutex);

    session->stat.bytesWritten += bytes;
    session->stat.packetsWritten += packets;
    ++session->stat.blocksWritten;
}

void Reservation_RawTsRecorder::AddCallbackFailure_(
    const std::shared_ptr<Session>& session) noexcept
{
    if (!session) {
        return;
    }

#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
    std::scoped_lock statGuard(session->statMutex);

    ++session->stat.callbackFailures;
}

void Reservation_RawTsRecorder::AddDiscardStats_(
    const std::shared_ptr<Session>& session,
    std::uint64_t blocks,
    std::uint64_t packets) noexcept
{
    if (!session) {
        return;
    }

#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
    std::scoped_lock statGuard(session->statMutex);

    session->stat.discardedBlocks += blocks;
    session->stat.discardedPackets += packets;
}

void Reservation_RawTsRecorder::MarkFailure_(
    const std::shared_ptr<Session>& session,
    const std::wstring& reason) noexcept
{
    if (!session) {
        return;
    }

#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
    std::scoped_lock statGuard(session->statMutex);

    session->stat.ok = false;
    session->stat.reason =
        reason.empty()
        ? L"内部録画処理で不明なエラーが発生しました。"
        : reason;
    ++session->stat.writeFailures;
}

void Reservation_RawTsRecorder::MarkNoDataTimeout_(
    const std::shared_ptr<Session>& session,
    const std::wstring& reason) noexcept
{
    if (!session) {
        return;
    }

#if defined(_MSC_VER)
#pragma warning(suppress: 26110)
#endif
    std::scoped_lock statGuard(session->statMutex);

    session->stat.ok = false;
    session->stat.noDataTimeout = true;

    if (session->stat.reason.empty()) {
        session->stat.reason =
            reason.empty()
            ? L"録画開始後、TSデータを受信できませんでした。"
            : reason;
    }
}