#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TVTestIntegration.hpp"
#include "DebugManager.h"

#include <Windows.h>
#include <filesystem>
#include <system_error>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <cctype>
#include <cwctype>

namespace
{
    constexpr DWORD kTvTestPipeOpenRetrySleepMs = 180;
    constexpr DWORD kTvTestPipeBusyWaitMs = 250;
    constexpr DWORD kTvTestPipeFailLogIntervalMs = 1500;

    ULONGLONG g_lastTvTestControlPipeOpenFailLogTick = 0;

    bool ShouldLogTvTestPipeOpenFail_() noexcept
    {
        const ULONGLONG now = ::GetTickCount64();

        if (g_lastTvTestControlPipeOpenFailLogTick == 0 ||
            now - g_lastTvTestControlPipeOpenFailLogTick >= kTvTestPipeFailLogIntervalMs) {
            g_lastTvTestControlPipeOpenFailLogTick = now;
            return true;
        }

        return false;
    }

    std::filesystem::path GetModuleDirectoryPath_()
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD len = ::GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(_countof(modulePath)));
        if (len == 0 || len >= _countof(modulePath)) {
            return {};
        }
        return std::filesystem::path(modulePath).parent_path();
    }

    std::wstring Utf8ToWide_(const std::string& src)
    {
        if (src.empty()) {
            return {};
        }

        const int needed = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            nullptr,
            0);

        if (needed <= 0) {
            return {};
        }

        std::wstring out(static_cast<size_t>(needed), L'\0');
        ::MultiByteToWideChar(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            out.data(),
            needed);

        return out;
    }

    std::string WideToUtf8_(std::wstring_view src)
    {
        if (src.empty()) {
            return {};
        }

        const int needed = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (needed <= 0) {
            return {};
        }

        std::string out(static_cast<size_t>(needed), '\0');
        ::WideCharToMultiByte(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            out.data(),
            needed,
            nullptr,
            nullptr);

        return out;
    }

    std::string ToUpperAscii_(std::string s)
    {
        for (char& ch : s) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        return s;
    }

    std::wstring ToUpperWide_(std::wstring s)
    {
        for (wchar_t& ch : s) {
            ch = static_cast<wchar_t>(std::towupper(ch));
        }
        return s;
    }

    std::wstring TrimWide_(const std::wstring& text)
    {
        if (text.empty()) {
            return {};
        }

        const std::size_t first = text.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) {
            return {};
        }

        const std::size_t last = text.find_last_not_of(L" \t\r\n");
        std::wstring out = text.substr(first, last - first + 1);

        if (out.size() >= 2 && out.front() == L'"' && out.back() == L'"') {
            out = out.substr(1, out.size() - 2);
        }

        return out;
    }

    void ReplaceAllWide_(
        std::wstring& text,
        const std::wstring& from,
        const std::wstring& to)
    {
        if (from.empty()) {
            return;
        }

        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::wstring::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    std::wstring MakeTvTestDidArgument_(int didIndex)
    {
        if (didIndex < 0) {
            return {};
        }

        if (didIndex > 25) {
            didIndex = 25;
        }

        wchar_t buf[8]{};
        _snwprintf_s(
            buf,
            _countof(buf),
            _TRUNCATE,
            L"%c",
            static_cast<wchar_t>(L'A' + didIndex));

        return buf;
    }

    bool ContainsDidSwitch_(const std::wstring& args)
    {
        const std::wstring upper = ToUpperWide_(args);

        return
            upper.find(L"/DID") != std::wstring::npos ||
            upper.find(L"-DID") != std::wstring::npos;
    }

    std::wstring ResolveTunerPipeTemplate_(std::wstring text, int didIndex)
    {
        text = TrimWide_(text);

        if (text.empty()) {
            return {};
        }

        int safeId = didIndex;
        if (safeId < 0) {
            safeId = 0;
        }
        if (safeId > 25) {
            safeId = 25;
        }

        const std::wstring number = std::to_wstring(safeId);

        wchar_t didBuf[8]{};
        _snwprintf_s(
            didBuf,
            _countof(didBuf),
            _TRUNCATE,
            L"%c",
            static_cast<wchar_t>(L'A' + safeId));

        const std::wstring didLetter = didBuf;

        ReplaceAllWide_(text, L"{TUNER}", number);
        ReplaceAllWide_(text, L"%TUNER%", number);

        ReplaceAllWide_(text, L"{X}", number);
        ReplaceAllWide_(text, L"%X%", number);

        ReplaceAllWide_(text, L"_X", L"_" + number);

        ReplaceAllWide_(text, L"{DID}", didLetter);
        ReplaceAllWide_(text, L"%DID%", didLetter);

        return text;
    }

    bool ReadPipeMessage_(HANDLE hPipe, std::vector<std::uint8_t>& out)
    {
        out.clear();

        if (hPipe == INVALID_HANDLE_VALUE || hPipe == nullptr) {
            return false;
        }

        constexpr DWORD kMaxMessageBytes =
            static_cast<DWORD>(sizeof(TVTestIntegration::TsFrameHeader) +
                TVTestIntegration::kMaxTsFramePayloadBytes);

        out.resize(kMaxMessageBytes);

        DWORD bytesRead = 0;
        const BOOL ok = ::ReadFile(
            hPipe,
            out.data(),
            kMaxMessageBytes,
            &bytesRead,
            nullptr);

        if (!ok) {
            const DWORD err = ::GetLastError();

            // 受信上限を超える message は安全のため失敗扱い
            if (err == ERROR_MORE_DATA) {
                DebugLogFormat(
                    L"[TVTestIntegration] ReadPipeMessage failed: message too large (err=%lu)",
                    static_cast<unsigned long>(err));
            }
            else {
                DebugLogFormat(
                    L"[TVTestIntegration] ReadPipeMessage failed: err=%lu",
                    static_cast<unsigned long>(err));
            }

            out.clear();
            return false;
        }

        out.resize(bytesRead);
        return true;
    }


}

bool TVTestIntegration::Configure(const Settings& settings)
{
    settings_ = settings;
    return true;
}

void TVTestIntegration::Reset() noexcept
{
    CloseTsPipeSession();
    settings_ = Settings{};
}

bool TVTestIntegration::IsEnabled() const noexcept
{
    return settings_.enabled;
}

const TVTestIntegration::Settings& TVTestIntegration::GetSettings() const noexcept
{
    return settings_;
}

bool TVTestIntegration::LaunchTVTest() const
{
    std::wstring exePath;
    if (!ValidateExecutablePath_(exePath)) {
        return false;
    }

    std::wstring commandLine = Quote_(exePath);

    // /DID は useDidSuffix とは分離する。
    // useDidSuffix=false でも、TVTest側プラグインの {TUNER} 判定には /DID が必要。
    const std::wstring didArg = MakeTvTestDidArgument_(settings_.didIndex);
    if (!didArg.empty() && !ContainsDidSwitch_(settings_.extraLaunchArgs)) {
        commandLine += L" /DID ";
        commandLine += didArg;
    }

    // 例: /nodshow
    if (!settings_.extraLaunchArgs.empty()) {
        commandLine += L" ";
        commandLine += settings_.extraLaunchArgs;
    }

    const std::wstring bonDriver = NormalizeText_(settings_.bonDriverPath);
    if (!bonDriver.empty()) {
        commandLine += L" /d ";
        commandLine += Quote_(bonDriver);
    }

    DebugLogFormat(
        L"[TVTestIntegration] LaunchTVTest: %ls",
        commandLine.c_str());

    return LaunchProcess_(exePath, commandLine);
}

bool TVTestIntegration::Ping(std::wstring* response) const
{
    return SendPipeCommand_(L"PING", response);
}

bool TVTestIntegration::QueryState(std::wstring& response) const
{
    response.clear();
    return SendPipeCommand_(L"GET_STATE", &response);
}

bool TVTestIntegration::QueryStatus(std::wstring& response) const
{
    response.clear();
    return SendPipeCommand_(L"GET_STATUS", &response);
}

bool TVTestIntegration::QueryProgram(std::wstring& response) const
{
    response.clear();
    return SendPipeCommand_(L"GET_PROGRAM", &response);
}

bool TVTestIntegration::EnableTs(bool enable, std::wstring* response) const
{
    return SendPipeCommand_(enable ? L"ENABLE_TS" : L"DISABLE_TS", response);
}

bool TVTestIntegration::SetChannel(int space, int channel, std::uint16_t serviceId, std::wstring* response) const
{
    wchar_t buf[128]{};
    if (serviceId != 0) {
        _snwprintf_s(
            buf,
            _countof(buf),
            _TRUNCATE,
            L"SET_CHANNEL %d %d %u",
            space,
            channel,
            static_cast<unsigned>(serviceId));
    }
    else {
        _snwprintf_s(
            buf,
            _countof(buf),
            _TRUNCATE,
            L"SET_CHANNEL %d %d",
            space,
            channel);
    }
    return SendPipeCommand_(buf, response);
}

bool TVTestIntegration::SetService(std::uint16_t serviceId, std::wstring* response) const
{
    wchar_t buf[64]{};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"SET_SERVICE %u", static_cast<unsigned>(serviceId));
    return SendPipeCommand_(buf, response);
}

bool TVTestIntegration::StartRecord(const std::wstring& filePath, std::wstring* response) const
{
    std::wstring cmd = L"START_RECORD";
    const std::wstring normalized = NormalizeText_(filePath);
    if (!normalized.empty()) {
        cmd += L" ";
        cmd += Quote_(normalized);
    }
    return SendPipeCommand_(cmd, response);
}

bool TVTestIntegration::StopRecord(std::wstring* response) const
{
    return SendPipeCommand_(L"STOP_RECORD", response);
}

bool TVTestIntegration::DoCommand(std::wstring_view commandName, std::wstring* response) const
{
    if (commandName.empty()) {
        return false;
    }

    std::wstring cmd = L"DO_COMMAND ";
    cmd.append(commandName.data(), commandName.size());
    return SendPipeCommand_(cmd, response);
}

bool TVTestIntegration::SetSilent(bool silent, std::wstring* response) const
{
    return SendPipeCommand_(silent ? L"SET_SILENT 1" : L"SET_SILENT 0", response);
}

bool TVTestIntegration::SetPreview(bool preview, std::wstring* response) const
{
    return SendPipeCommand_(preview ? L"SET_PREVIEW 1" : L"SET_PREVIEW 0", response);
}

bool TVTestIntegration::ReloadPluginIni(std::wstring* response) const
{
    return SendPipeCommand_(L"RELOAD_INI", response);
}

bool TVTestIntegration::CloseTVTest(std::wstring* response) const
{
    return SendPipeCommand_(L"CLOSE", response);
}

bool TVTestIntegration::SendRawCommand(std::wstring_view command, std::wstring* response) const
{
    if (command.empty()) {
        return false;
    }
    return SendPipeCommand_(command, response);
}

bool TVTestIntegration::OpenTsPipeSession()
{
    CloseTsPipeSession();

    HANDLE hPipe = INVALID_HANDLE_VALUE;
    if (!OpenTsPipeHandle_(hPipe)) {
        DebugLogFormat(L"[TVTestIntegration] OpenTsPipeSession failed: %ls", BuildTsPipeName_().c_str());
        return false;
    }

    tsPipeHandle_ = hPipe;
    DebugLogFormat(L"[TVTestIntegration] TS pipe session opened: %ls", BuildTsPipeName_().c_str());
    return true;
}

void TVTestIntegration::CloseTsPipeSession() noexcept
{
    if (tsPipeHandle_ != INVALID_HANDLE_VALUE && tsPipeHandle_ != nullptr) {
        ::CloseHandle(tsPipeHandle_);
        tsPipeHandle_ = INVALID_HANDLE_VALUE;
    }
}

bool TVTestIntegration::IsTsPipeSessionOpen() const noexcept
{
    return tsPipeHandle_ != INVALID_HANDLE_VALUE && tsPipeHandle_ != nullptr;
}

bool TVTestIntegration::ReadNextTsFrame(TsFrame& frame)
{
    frame = TsFrame{};

    if (!IsTsPipeSessionOpen()) {
        DebugLogFormat(L"[TVTestIntegration] ReadNextTsFrame failed: TS session not open");
        return false;
    }

    std::vector<std::uint8_t> message;
    if (!ReadPipeMessage_(tsPipeHandle_, message)) {
        DebugLogFormat(L"[TVTestIntegration] ReadNextTsFrame message read failed");
        CloseTsPipeSession();
        return false;
    }

    if (message.size() < sizeof(TsFrameHeader)) {
        DebugLogFormat(
            L"[TVTestIntegration] ReadNextTsFrame short message: bytes=%zu",
            message.size());
        CloseTsPipeSession();
        return false;
    }

    std::memcpy(&frame.header, message.data(), sizeof(TsFrameHeader));

    if (!IsTsFrameHeaderValid(frame.header)) {
        DebugLogFormat(
            L"[TVTestIntegration] invalid TS frame header: magic=0x%08X payloadBytes=%u flags=0x%08X",
            static_cast<unsigned>(frame.header.magic),
            static_cast<unsigned>(frame.header.payloadBytes),
            static_cast<unsigned>(frame.header.flags));
        CloseTsPipeSession();
        return false;
    }

    const std::size_t requiredBytes =
        sizeof(TsFrameHeader) + static_cast<std::size_t>(frame.header.payloadBytes);

    if (message.size() < requiredBytes) {
        DebugLogFormat(
            L"[TVTestIntegration] TS message too short: actual=%zu required=%zu",
            message.size(),
            requiredBytes);
        CloseTsPipeSession();
        return false;
    }

    frame.payload.assign(
        message.begin() + static_cast<std::ptrdiff_t>(sizeof(TsFrameHeader)),
        message.begin() + static_cast<std::ptrdiff_t>(requiredBytes));

    if (!IsTsPayloadAligned188(frame)) {
        DebugLogFormat(
            L"[TVTestIntegration] TS payload is not 188-byte aligned: payloadBytes=%u",
            static_cast<unsigned>(frame.header.payloadBytes));
    }

    return true;
}

bool TVTestIntegration::ReadNextTsFrameWithTimeout(TsFrame& frame, DWORD waitTimeoutMs)
{
    frame = TsFrame{};

    if (!IsTsPipeSessionOpen()) {
        return false;
    }

    const ULONGLONG deadline = ::GetTickCount64() + waitTimeoutMs;

    while (::GetTickCount64() < deadline) {
        TsFrameHeader peekHeader{};
        DWORD bytesRead = 0;
        DWORD totalAvail = 0;
        DWORD bytesLeftThisMessage = 0;

        if (!::PeekNamedPipe(
            tsPipeHandle_,
            &peekHeader,
            static_cast<DWORD>(sizeof(peekHeader)),
            &bytesRead,
            &totalAvail,
            &bytesLeftThisMessage)) {
            DebugLogFormat(L"[TVTestIntegration] PeekNamedPipe failed during TS wait");
            CloseTsPipeSession();
            return false;
        }

        if (bytesRead >= sizeof(TsFrameHeader)) {
            const ULONGLONG messageBytes =
                static_cast<ULONGLONG>(bytesRead) +
                static_cast<ULONGLONG>(bytesLeftThisMessage);

            const ULONGLONG requiredBytes =
                static_cast<ULONGLONG>(sizeof(TsFrameHeader)) +
                static_cast<ULONGLONG>(peekHeader.payloadBytes);

            if (messageBytes >= requiredBytes) {
                return ReadNextTsFrame(frame);
            }
        }

        ::Sleep(10);
    }

    return false;
}

bool TVTestIntegration::IsTsFrameHeaderValid(const TsFrameHeader& header) noexcept
{
    if (header.magic != kTsFrameMagic) {
        return false;
    }
    if (header.payloadBytes > kMaxTsFramePayloadBytes) {
        return false;
    }
    return true;
}

bool TVTestIntegration::IsTsPayloadAligned188(const TsFrame& frame) noexcept
{
    if (frame.header.payloadBytes == 0) {
        return true;
    }
    if (frame.payload.size() != frame.header.payloadBytes) {
        return false;
    }
    return (frame.header.payloadBytes % 188u) == 0u;
}

std::size_t TVTestIntegration::GetTsPacketCount(const TsFrame& frame) noexcept
{
    if (!IsTsPayloadAligned188(frame)) {
        return 0;
    }
    return static_cast<std::size_t>(frame.header.payloadBytes / 188u);
}

bool TVTestIntegration::ValidateExecutablePath_(std::wstring& exePath) const
{
    exePath = NormalizeText_(settings_.tvtestExePath);
    if (exePath.empty()) {
        DebugLogFormat(L"[TVTestIntegration] TVTest exe path is empty");
        return false;
    }

    std::filesystem::path p(exePath);
    if (p.is_relative()) {
        const std::filesystem::path moduleDir = GetModuleDirectoryPath_();
        if (!moduleDir.empty()) {
            p = moduleDir / p;
        }
    }

    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) {
        DebugLogFormat(L"[TVTestIntegration] TVTest exe not found: %ls", p.wstring().c_str());
        return false;
    }

    exePath = p.wstring();
    return true;
}

bool TVTestIntegration::LaunchProcess_(const std::wstring& exePath, const std::wstring& commandLine) const
{
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi{};
    std::filesystem::path workingDir;
    try {
        workingDir = std::filesystem::path(exePath).parent_path();
    }
    catch (...) {
        workingDir.clear();
    }

    const BOOL ok = ::CreateProcessW(
        exePath.c_str(),
        cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &si,
        &pi);

    if (!ok) {
        const DWORD errorCode = ::GetLastError();
        DebugLogFormat(
            L"[TVTestIntegration] CreateProcessW failed: path=%ls error=%lu",
            exePath.c_str(),
            static_cast<unsigned long>(errorCode));
        return false;
    }

    ::CloseHandle(pi.hThread);

    if (pi.hProcess != nullptr) {
        if (settings_.launchWaitMs > 0) {
            ::WaitForInputIdle(pi.hProcess, settings_.launchWaitMs);
        }
        ::CloseHandle(pi.hProcess);
    }

    return true;
}

bool TVTestIntegration::SendPipeCommand_(std::wstring_view command, std::wstring* response) const
{
    if (!settings_.enabled) {
        DebugLogFormat(L"[TVTestIntegration] command ignored because integration is disabled");
        return false;
    }

    HANDLE hPipe = INVALID_HANDLE_VALUE;
    const std::wstring pipeName = BuildControlPipeName_();

    if (!OpenPipeHandle_(pipeName, settings_.pipeTimeoutMs, hPipe)) {
        DebugLogFormat(L"[TVTestIntegration] control pipe open failed: %ls", pipeName.c_str());
        return false;
    }

    bool ok = WriteUtf8Line_(hPipe, command);
    if (ok && response != nullptr) {
        const DWORD timeout = (settings_.pipeTimeoutMs < 3000) ? 3000 : settings_.pipeTimeoutMs;
        ok = ReadUtf8Response_(hPipe, command, timeout, response);
    }
    else if (response != nullptr) {
        response->clear();
    }

    ::CloseHandle(hPipe);
    return ok;
}

bool TVTestIntegration::OpenPipeHandle_(const std::wstring& pipeName, DWORD timeoutMs, HANDLE& hPipe) const
{
    hPipe = INVALID_HANDLE_VALUE;

    if (pipeName.empty()) {
        return false;
    }

    hPipe = ::CreateFileW(
        pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_PIPE_BUSY) {
            return false;
        }

        if (!::WaitNamedPipeW(pipeName.c_str(), timeoutMs)) {
            return false;
        }

        hPipe = ::CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            return false;
        }
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!::SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr)) {
        ::CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
        return false;
    }

    return true;
}

bool TVTestIntegration::OpenTsPipeHandle_(HANDLE& hPipe) const
{
    return OpenPipeHandle_(BuildTsPipeName_(), settings_.pipeTimeoutMs, hPipe);
}

std::wstring TVTestIntegration::BuildControlPipeName_() const
{
    std::wstring pipeName =
        ResolveTunerPipeTemplate_(
            settings_.controlPipeName,
            settings_.didIndex);

    if (settings_.useDidSuffix) {
        pipeName += BuildDidSuffix_();
    }

    return pipeName;
}

std::wstring TVTestIntegration::BuildTsPipeName_() const
{
    std::wstring pipeName =
        ResolveTunerPipeTemplate_(
            settings_.tsPipeName,
            settings_.didIndex);

    if (settings_.useDidSuffix) {
        pipeName += BuildDidSuffix_();
    }

    return pipeName;
}

std::wstring TVTestIntegration::BuildDidSuffix_() const
{
    if (!settings_.useDidSuffix) {
        return {};
    }

    if (settings_.didIndex < 0 || settings_.didIndex > 25) {
        return {};
    }

    wchar_t suffix[8]{};
    _snwprintf_s(
        suffix,
        _countof(suffix),
        _TRUNCATE,
        L"_%c",
        static_cast<wchar_t>(L'A' + settings_.didIndex));

    return suffix;
}

std::wstring TVTestIntegration::NormalizeText_(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }

    size_t last = text.find_last_not_of(L" \t\r\n");
    std::wstring out = text.substr(first, last - first + 1);

    if (out.size() >= 2 && out.front() == L'"' && out.back() == L'"') {
        out = out.substr(1, out.size() - 2);
    }

    return out;
}

std::wstring TVTestIntegration::Quote_(const std::wstring& text)
{
    std::wstring out = L"\"";
    for (wchar_t ch : text) {
        if (ch == L'"') {
            out += L"\\\"";
        }
        else {
            out.push_back(ch);
        }
    }
    out += L"\"";
    return out;
}

bool TVTestIntegration::WriteUtf8Line_(HANDLE hPipe, std::wstring_view text)
{
    if (hPipe == INVALID_HANDLE_VALUE || hPipe == nullptr) {
        return false;
    }

    std::wstring line(text);
    if (line.empty() || line.back() != L'\n') {
        line.push_back(L'\n');
    }

    const std::string utf8 = WideToUtf8_(line);
    if (utf8.empty()) {
        return false;
    }

    DWORD written = 0;
    return ::WriteFile(
        hPipe,
        utf8.data(),
        static_cast<DWORD>(utf8.size()),
        &written,
        nullptr) && written == utf8.size();
}

bool TVTestIntegration::ReadUtf8Response_(HANDLE hPipe, std::wstring_view command, DWORD timeoutMs, std::wstring* response)
{
    if (response == nullptr) {
        return false;
    }

    response->clear();

    std::string pending;
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;

    while (::GetTickCount64() < deadline) {
        char buffer[4096]{};
        DWORD bytesRead = 0;

        const BOOL ok = ::ReadFile(
            hPipe,
            buffer,
            static_cast<DWORD>(sizeof(buffer) - 1),
            &bytesRead,
            nullptr);

        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_MORE_DATA && bytesRead > 0) {
                pending.append(buffer, buffer + bytesRead);
            }
            else {
                DebugLogFormat(
                    L"[TVTestIntegration] ReadFile failed for command=%ls err=%lu",
                    std::wstring(command).c_str(),
                    static_cast<unsigned long>(err));
                return false;
            }
        }
        else {
            if (bytesRead == 0) {
                break;
            }
            pending.append(buffer, buffer + bytesRead);
        }

        for (;;) {
            const size_t eol = pending.find('\n');
            if (eol == std::string::npos) {
                break;
            }

            std::string one = pending.substr(0, eol);
            pending.erase(0, eol + 1);

            while (!one.empty() && (one.back() == '\r' || one.back() == '\n')) {
                one.pop_back();
            }
            if (one.empty()) {
                continue;
            }

            const std::wstring wline = Utf8ToWide_(one);
            DebugLogFormat(L"[TVTestIntegration] recv: %ls", wline.c_str());

            if (ShouldSkipControlLine_(one)) {
                continue;
            }

            if (!IsExpectedReplyForCommand_(command, one)) {
                continue;
            }

            *response = wline;
            return true;
        }
    }

    DebugLogFormat(
        L"[TVTestIntegration] response timeout/no-match for command=%ls",
        std::wstring(command).c_str());

    return false;
}

bool TVTestIntegration::ReadExact_(HANDLE hPipe, void* buffer, DWORD bytes)
{
    std::uint8_t* dst = static_cast<std::uint8_t*>(buffer);
    DWORD total = 0;

    while (total < bytes) {
        DWORD chunk = 0;
        if (!::ReadFile(hPipe, dst + total, bytes - total, &chunk, nullptr) || chunk == 0) {
            return false;
        }
        total += chunk;
    }

    return true;
}

bool TVTestIntegration::ShouldSkipControlLine_(const std::string& lineUtf8)
{
    if (lineUtf8.rfind("HELLO", 0) == 0) {
        return true;
    }
    if (lineUtf8.rfind("SNAP reason=\"CONNECTED\"", 0) == 0) {
        return true;
    }
    return false;
}

bool TVTestIntegration::IsExpectedReplyForCommand_(
    std::wstring_view command,
    const std::string& lineUtf8)
{
    const std::string cmd = ToUpperAscii_(WideToUtf8_(command));

    auto startsWith = [](const std::string& s, const char* prefix) -> bool {
        return s.rfind(prefix, 0) == 0;
        };

    auto isOkOrErr = [&]() -> bool {
        return lineUtf8.rfind("OK", 0) == 0 ||
            lineUtf8.rfind("ERR", 0) == 0;
        };

    if (cmd == "PING") {
        return lineUtf8.rfind("OK PONG", 0) == 0;
    }

    if (cmd == "GET_STATE") {
        return lineUtf8.rfind("SNAP reason=\"GET_STATE\"", 0) == 0;
    }

    if (cmd == "GET_STATUS") {
        return lineUtf8.rfind("STATUS ", 0) == 0;
    }

    if (cmd == "GET_PROGRAM") {
        return lineUtf8.rfind("PROGRAM ", 0) == 0;
    }

    if (cmd == "ENABLE_TS") {
        return lineUtf8.rfind("OK TS enabled", 0) == 0;
    }

    if (cmd == "DISABLE_TS") {
        return lineUtf8.rfind("OK TS disabled", 0) == 0;
    }

    if (startsWith(cmd, "SET_CHANNEL_BY_IDS ")) {
        if (isOkOrErr()) {
            return true;
        }

        if (lineUtf8.rfind("SNAP reason=\"SET_CHANNEL_BY_IDS\"", 0) == 0) {
            return true;
        }

        if (lineUtf8.rfind("SNAP reason=\"SET_CHANNEL\"", 0) == 0) {
            return true;
        }

        if (lineUtf8.find("SET_CHANNEL_BY_IDS") != std::string::npos) {
            return true;
        }

        return false;
    }

    if (startsWith(cmd, "SET_CHANNEL ")) {
        if (isOkOrErr()) {
            return true;
        }

        if (lineUtf8.rfind("SNAP reason=\"SET_CHANNEL\"", 0) == 0) {
            return true;
        }

        return false;
    }

    if (startsWith(cmd, "SET_SERVICE ")) {
        if (isOkOrErr()) {
            return true;
        }

        if (lineUtf8.rfind("SNAP reason=\"SET_SERVICE\"", 0) == 0) {
            return true;
        }

        return false;
    }

    if (startsWith(cmd, "START_RECORD") ||
        startsWith(cmd, "STOP_RECORD") ||
        startsWith(cmd, "DO_COMMAND ") ||
        startsWith(cmd, "SET_SILENT ") ||
        startsWith(cmd, "SET_PREVIEW ") ||
        startsWith(cmd, "RELOAD_INI") ||
        startsWith(cmd, "CLOSE")) {
        return isOkOrErr();
    }

    return isOkOrErr();
}