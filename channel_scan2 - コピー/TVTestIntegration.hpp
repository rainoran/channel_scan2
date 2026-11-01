#pragma once
#ifndef TVTEST_INTEGRATION_HPP
#define TVTEST_INTEGRATION_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>

class TVTestIntegration
{
public:
    struct Settings
    {
        bool enabled = false;

        // 旧方式用。
        // true にすると BuildControlPipeName_() / BuildTsPipeName_() で _A/_B を後付けする。
        // 新方式 TVTestBridge_{TUNER} / TVTestBridge_0 / _1 では false。
        bool useDidSuffix = false;

        bool launchUseBonDriverCommand = false;
        std::wstring tvtestWorkingDirectory;
        std::wstring tvtestBonDriverCommandName;

        // 0=A, 1=B, ...
        // これは useDidSuffix=false でも TVTest 起動時の /DID A/B に使う。
        int didIndex = -1;

        std::wstring tvtestExePath;
        std::wstring bonDriverPath;

        // 新方式の既定値。
        // {TUNER} は didIndex から 0/1/2... に置換する。
        std::wstring controlPipeName = L"\\\\.\\pipe\\TVTestBridge_{TUNER}";
        std::wstring tsPipeName = L"\\\\.\\pipe\\TVTestBridgeTS_{TUNER}";

        // 例: /nodshow
        std::wstring extraLaunchArgs;

        DWORD pipeTimeoutMs = 1500;
        DWORD launchWaitMs = 3000;
    };

    struct TsFrameHeader
    {
        std::uint32_t magic = 0;
        std::uint32_t payloadBytes = 0;
        std::uint32_t flags = 0;
        std::uint32_t reserved = 0;
    };

    struct TsFrame
    {
        TsFrameHeader header{};
        std::vector<std::uint8_t> payload;
    };

public:
    static constexpr std::uint32_t kTsFrameMagic = 0x31534254u;              // 'TSB1'
    static constexpr std::uint32_t kMaxTsFramePayloadBytes = 188u * 4096u;   // 安全側で制限

public:
    TVTestIntegration() = default;
    ~TVTestIntegration() = default;

    bool Configure(const Settings& settings);
    void Reset() noexcept;

    bool IsEnabled() const noexcept;
    const Settings& GetSettings() const noexcept;

    bool LaunchTVTest() const;

    bool Ping(std::wstring* response = nullptr) const;
    bool QueryState(std::wstring& response) const;
    bool QueryStatus(std::wstring& response) const;
    bool QueryProgram(std::wstring& response) const;

    bool EnableTs(bool enable, std::wstring* response = nullptr) const;
    bool SetChannel(int space, int channel, std::uint16_t serviceId = 0, std::wstring* response = nullptr) const;
    bool SetService(std::uint16_t serviceId, std::wstring* response = nullptr) const;
    bool StartRecord(const std::wstring& filePath = L"", std::wstring* response = nullptr) const;
    bool StopRecord(std::wstring* response = nullptr) const;
    bool DoCommand(std::wstring_view commandName, std::wstring* response = nullptr) const;
    bool SetSilent(bool silent, std::wstring* response = nullptr) const;
    bool SetPreview(bool preview, std::wstring* response = nullptr) const;
    bool ReloadPluginIni(std::wstring* response = nullptr) const;
    bool CloseTVTest(std::wstring* response = nullptr) const;

    bool SendRawCommand(std::wstring_view command, std::wstring* response = nullptr) const;

    // ---- TS パイプ手動セッション ----
    bool OpenTsPipeSession();
    void CloseTsPipeSession() noexcept;
    bool IsTsPipeSessionOpen() const noexcept;
    bool ReadNextTsFrame(TsFrame& frame);
    bool ReadNextTsFrameWithTimeout(TsFrame& frame, DWORD waitTimeoutMs);

    static bool IsTsFrameHeaderValid(const TsFrameHeader& header) noexcept;
    static bool IsTsPayloadAligned188(const TsFrame& frame) noexcept;
    static std::size_t GetTsPacketCount(const TsFrame& frame) noexcept;

private:
    bool ValidateExecutablePath_(std::wstring& exePath) const;
    bool LaunchProcess_(const std::wstring& exePath, const std::wstring& commandLine) const;

    bool SendPipeCommand_(std::wstring_view command, std::wstring* response) const;
    bool OpenPipeHandle_(const std::wstring& pipeName, DWORD timeoutMs, HANDLE& hPipe) const;
    bool OpenTsPipeHandle_(HANDLE& hPipe) const;

    std::wstring BuildControlPipeName_() const;
    std::wstring BuildTsPipeName_() const;
    std::wstring BuildDidSuffix_() const;

    static std::wstring NormalizeText_(const std::wstring& text);
    static std::wstring Quote_(const std::wstring& text);

    static bool WriteUtf8Line_(HANDLE hPipe, std::wstring_view text);
    static bool ReadUtf8Response_(HANDLE hPipe, std::wstring_view command, DWORD timeoutMs, std::wstring* response);
    static bool ReadExact_(HANDLE hPipe, void* buffer, DWORD bytes);

    static bool ShouldSkipControlLine_(const std::string& lineUtf8);
    static bool IsExpectedReplyForCommand_(std::wstring_view command, const std::string& lineUtf8);

private:
    Settings settings_{};
    HANDLE tsPipeHandle_ = INVALID_HANDLE_VALUE;
};

#endif // TVTEST_INTEGRATION_HPP