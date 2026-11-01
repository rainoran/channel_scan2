#pragma once
#ifndef TSCEPG_PLUGIN_SDK_HPP
#define TSCEPG_PLUGIN_SDK_HPP

#include <cstddef>
#include <cstdint>
#include <cwchar>

#ifndef TSCEPG_PLUGIN_CALL
#define TSCEPG_PLUGIN_CALL __cdecl
#endif

#if defined(_WIN32)
    #if defined(TSCEPG_PLUGIN_EXPORTS)
        #define TSCEPG_PLUGIN_EXPORT __declspec(dllexport)
    #else
        #define TSCEPG_PLUGIN_EXPORT __declspec(dllimport)
    #endif
#else
    #define TSCEPG_PLUGIN_EXPORT
#endif

#define TSCEPG_PLUGIN_EXTERN extern "C"

inline constexpr std::uint32_t TSCEPG_PLUGIN_ABI_VERSION = 0x00010000u;

inline constexpr std::uint32_t TSCEPG_PLUGIN_ID_MAX          = 64u;
inline constexpr std::uint32_t TSCEPG_PLUGIN_NAME_MAX        = 64u;
inline constexpr std::uint32_t TSCEPG_PLUGIN_AUTHOR_MAX      = 64u;
inline constexpr std::uint32_t TSCEPG_PLUGIN_VERSION_STR_MAX = 32u;
inline constexpr std::uint32_t TSCEPG_PLUGIN_DESC_MAX        = 256u;
inline constexpr std::uint32_t TSCEPG_PLUGIN_SUMMARY_MAX     = 256u;

enum class TSCEPG_LogLevel : std::uint32_t
{
    Trace = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3
};

enum class TSCEPG_PluginKind : std::uint32_t
{
    Unknown         = 0,
    UiExtension     = 1,
    EpgProcessor    = 2,
    StreamProcessor = 3,
    Utility         = 4
};

enum class TSCEPG_PathKind : std::uint32_t
{
    ExeDir     = 0,
    ConfigIni  = 1,
    PluginsDir = 2
};

enum TSCEPG_PluginFlags : std::uint32_t
{
    TSCEPG_PLUGIN_FLAG_NONE                 = 0u,
    TSCEPG_PLUGIN_FLAG_HAS_PANEL_SUMMARY    = 1u << 0,
    TSCEPG_PLUGIN_FLAG_SAFE_METADATA_PROBE  = 1u << 1,
    TSCEPG_PLUGIN_FLAG_SUPPORTS_COMMAND     = 1u << 2
};

using TSCEPG_PluginHandle = void*;

struct TSCEPG_PluginInfo final
{
    std::uint32_t structSize = sizeof(TSCEPG_PluginInfo);
    std::uint32_t abiVersion = TSCEPG_PLUGIN_ABI_VERSION;
    std::uint32_t pluginKind = static_cast<std::uint32_t>(TSCEPG_PluginKind::Unknown);
    std::uint32_t flags      = TSCEPG_PLUGIN_FLAG_NONE;

    wchar_t pluginId[TSCEPG_PLUGIN_ID_MAX]{};
    wchar_t displayName[TSCEPG_PLUGIN_NAME_MAX]{};
    wchar_t author[TSCEPG_PLUGIN_AUTHOR_MAX]{};
    wchar_t version[TSCEPG_PLUGIN_VERSION_STR_MAX]{};
    wchar_t description[TSCEPG_PLUGIN_DESC_MAX]{};
};

struct TSCEPG_HostApi final
{
    std::uint32_t structSize = sizeof(TSCEPG_HostApi);
    std::uint32_t abiVersion = TSCEPG_PLUGIN_ABI_VERSION;

    void (TSCEPG_PLUGIN_CALL* Log)(
        std::uint32_t level,
        const wchar_t* text) = nullptr;

    bool (TSCEPG_PLUGIN_CALL* GetPath)(
        std::uint32_t pathKind,
        wchar_t* outPath,
        std::uint32_t cchOutPath) = nullptr;

    bool (TSCEPG_PLUGIN_CALL* ReadIniString)(
        const wchar_t* section,
        const wchar_t* key,
        const wchar_t* defaultValue,
        wchar_t* outValue,
        std::uint32_t cchOutValue) = nullptr;

    bool (TSCEPG_PLUGIN_CALL* WriteIniString)(
        const wchar_t* section,
        const wchar_t* key,
        const wchar_t* value) = nullptr;

    void* reserved0 = nullptr;
    void* reserved1 = nullptr;
};

inline void TSCEPG_CopyWideString(
    wchar_t* dst,
    std::uint32_t cchDst,
    const wchar_t* src) noexcept
{
    if (dst == nullptr || cchDst == 0) {
        return;
    }

    dst[0] = L'\0';

    if (src == nullptr) {
        return;
    }

    std::uint32_t i = 0;
    for (; i + 1 < cchDst && src[i] != L'\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = L'\0';
}

TSCEPG_PLUGIN_EXTERN
TSCEPG_PLUGIN_EXPORT
bool TSCEPG_PLUGIN_CALL
TSCEPG_GetPluginInfo(TSCEPG_PluginInfo* outInfo) noexcept;

TSCEPG_PLUGIN_EXTERN
TSCEPG_PLUGIN_EXPORT
bool TSCEPG_PLUGIN_CALL
TSCEPG_CreatePlugin(
    const TSCEPG_HostApi* hostApi,
    TSCEPG_PluginHandle* outHandle) noexcept;

TSCEPG_PLUGIN_EXTERN
TSCEPG_PLUGIN_EXPORT
void TSCEPG_PLUGIN_CALL
TSCEPG_DestroyPlugin(TSCEPG_PluginHandle handle) noexcept;

TSCEPG_PLUGIN_EXTERN
TSCEPG_PLUGIN_EXPORT
bool TSCEPG_PLUGIN_CALL
TSCEPG_Initialize(TSCEPG_PluginHandle handle) noexcept;

TSCEPG_PLUGIN_EXTERN
TSCEPG_PLUGIN_EXPORT
void TSCEPG_PLUGIN_CALL
TSCEPG_Shutdown(TSCEPG_PluginHandle handle) noexcept;

TSCEPG_PLUGIN_EXTERN
TSCEPG_PLUGIN_EXPORT
bool TSCEPG_PLUGIN_CALL
TSCEPG_ReloadSettings(
    TSCEPG_PluginHandle handle,
    const wchar_t* iniPath) noexcept;

TSCEPG_PLUGIN_EXTERN
TSCEPG_PLUGIN_EXPORT
bool TSCEPG_PLUGIN_CALL
TSCEPG_GetPanelSummary(
    TSCEPG_PluginHandle handle,
    wchar_t* outText,
    std::uint32_t cchOutText) noexcept;

TSCEPG_PLUGIN_EXTERN
TSCEPG_PLUGIN_EXPORT
bool TSCEPG_PLUGIN_CALL
TSCEPG_ExecuteCommand(
    TSCEPG_PluginHandle handle,
    const wchar_t* command,
    const void* data,
    std::uint32_t dataSize) noexcept;

#endif // TSCEPG_PLUGIN_SDK_HPP