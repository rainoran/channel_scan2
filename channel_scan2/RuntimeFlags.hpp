#pragma once
#pragma once
#ifndef RUNTIME_FLAGS_HPP
#define RUNTIME_FLAGS_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <string>
#include <string_view>
#include <atomic>

#include "ConfigManager.h"
#include "DebugManager.h"

// 実行時フラグの「確定値」を持つ（起動時/保存時に更新）
namespace RuntimeFlags
{
    // ---- 内部：ini 読み取り ----
    inline bool ReadBool(const std::wstring& iniPath,
        const wchar_t* section, const wchar_t* key, bool defVal) noexcept
    {
        if (iniPath.empty() || !section || !key) return defVal;
        const UINT v = GetPrivateProfileIntW(section, key, defVal ? 1u : 0u, iniPath.c_str());
        return v != 0u;
    }

    // ---- 確定状態（メモリ上）----
    // Debug（安全に即時反映OK）
    inline std::atomic_bool g_DebugWriteToFile{ false }; // 参考用（実際の判定は ConfigManager を見る実装でもOK）
    inline std::atomic_bool g_EnableSDT{ false };
    inline std::atomic_bool g_EnableNIT{ false };

    // Arib（原則：次のデコードから反映でOK）
    inline std::atomic_bool g_AribEnableTrace{ false };
    inline std::atomic_bool g_AribTraceG{ false };
    inline std::atomic_bool g_AribTraceChars{ false };

    // ChannelScan Dump（危険：処理中の切替はしない。開始時スナップショットで固定）
    struct ChannelScanDumpSnapshot
    {
        bool enableTsDump = false;
        bool enableRingDump = false;
    };

    inline ChannelScanDumpSnapshot ReadChannelScanDumpSnapshotFromIni(const std::wstring& iniPath) noexcept
    {
        ChannelScanDumpSnapshot s{};
        s.enableTsDump = ReadBool(iniPath, L"ChannelScan", L"EnableTsDump", false);
        s.enableRingDump = ReadBool(iniPath, L"ChannelScan", L"EnableRingDump", false);
        return s;
    }

    // ---- 反映（Apply）----
    // 起動時：ini を確定 → グローバルへ反映（CUI互換固定）
    inline void ApplyAtStartup()
    {
        auto& cfg = ConfigManager::GetInstance();
        cfg.LoadConfig(); // WriteToFile / LogEncoding 等を確実に同期

        const std::wstring ini = cfg.GetIniPath();

        // Debug
        g_DebugWriteToFile.store(cfg.GetDebugWriteToFile(), std::memory_order_relaxed);

        const bool sdt = ReadBool(ini, L"Debug", L"EnableSDTDebug", false);
        const bool nit = ReadBool(ini, L"Debug", L"EnableNITDebug", false);
        g_EnableSDT.store(sdt, std::memory_order_relaxed);
        g_EnableNIT.store(nit, std::memory_order_relaxed);

        // 既存のグローバル（フィルターマクロが見る場所）へ反映
        g_EnableSDTDebug.store(sdt, std::memory_order_relaxed);
        g_EnableNITDebug.store(nit, std::memory_order_relaxed);

        // Arib
        const bool aribTrace = ReadBool(ini, L"Arib", L"EnableTrace", false);
        const bool traceG = ReadBool(ini, L"Arib", L"TraceG", false);
        const bool traceCh = ReadBool(ini, L"Arib", L"TraceChars", false);

        g_AribEnableTrace.store(aribTrace, std::memory_order_relaxed);
        g_AribTraceG.store(traceG, std::memory_order_relaxed);
        g_AribTraceChars.store(traceCh, std::memory_order_relaxed);

        // DebugManager の内部キャッシュ更新（IsDebugEnabled() 等の整合）
        DebugManager::GetInstance().ConfigureDebugMode();

        DebugLog(L"[CFG] ApplyAtStartup: WriteToFile=%d SDT=%d NIT=%d AribTrace=%d (G=%d Ch=%d)",
            cfg.GetDebugWriteToFile() ? 1 : 0,
            sdt ? 1 : 0,
            nit ? 1 : 0,
            aribTrace ? 1 : 0,
            traceG ? 1 : 0,
            traceCh ? 1 : 0
        );
    }

    // 保存時：再固定（安全なものだけ）
    // ※ Dump系は “ここでは確定しても良いが、処理中切替はしない” → Scan開始時スナップショットで固定する
    inline void ApplyAfterConfigSaved()
    {
        auto& cfg = ConfigManager::GetInstance();
        cfg.LoadConfig(); // UIが ini を書き換えた後でも同期

        const std::wstring ini = cfg.GetIniPath();

        // Debug
        g_DebugWriteToFile.store(cfg.GetDebugWriteToFile(), std::memory_order_relaxed);

        const bool sdt = ReadBool(ini, L"Debug", L"EnableSDTDebug", false);
        const bool nit = ReadBool(ini, L"Debug", L"EnableNITDebug", false);

        g_EnableSDT.store(sdt, std::memory_order_relaxed);
        g_EnableNIT.store(nit, std::memory_order_relaxed);

        g_EnableSDTDebug.store(sdt, std::memory_order_relaxed);
        g_EnableNITDebug.store(nit, std::memory_order_relaxed);

        // Arib
        const bool aribTrace = ReadBool(ini, L"Arib", L"EnableTrace", false);
        const bool traceG = ReadBool(ini, L"Arib", L"TraceG", false);
        const bool traceCh = ReadBool(ini, L"Arib", L"TraceChars", false);

        g_AribEnableTrace.store(aribTrace, std::memory_order_relaxed);
        g_AribTraceG.store(traceG, std::memory_order_relaxed);
        g_AribTraceChars.store(traceCh, std::memory_order_relaxed);

        DebugManager::GetInstance().ConfigureDebugMode();

        DebugLog(L"[CFG] ApplyAfterConfigSaved: WriteToFile=%d SDT=%d NIT=%d AribTrace=%d (G=%d Ch=%d)",
            cfg.GetDebugWriteToFile() ? 1 : 0,
            sdt ? 1 : 0,
            nit ? 1 : 0,
            aribTrace ? 1 : 0,
            traceG ? 1 : 0,
            traceCh ? 1 : 0
        );
    }

    // AribDecodecallUnit に trace を確定値で適用（ユニット生成後に1回呼ぶ）
    template <class TAribDecodeUnit>
    inline void ApplyAribTraceTo(TAribDecodeUnit& unit) noexcept
    {
        // EnableTrace=1 または TraceG/TraceChars を使うなら true 扱い
        const bool enable =
            g_AribEnableTrace.load(std::memory_order_relaxed) ||
            g_AribTraceG.load(std::memory_order_relaxed) ||
            g_AribTraceChars.load(std::memory_order_relaxed);

        unit.EnableTraceForTest(enable);
    }

    // ChannelScan開始時にDumpを固定する（処理中切替事故を防止）
    inline ChannelScanDumpSnapshot GetChannelScanDumpSnapshot()
    {
        const std::wstring ini = ConfigManager::GetInstance().GetIniPath();
        return ReadChannelScanDumpSnapshotFromIni(ini);
    }
}

#endif // RUNTIME_FLAGS_HPP
