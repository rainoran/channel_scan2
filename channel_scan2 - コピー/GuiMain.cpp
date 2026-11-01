// GuiMain.cpp  (C++20)
// - config.ini を exe と同じフォルダに必ず作る/読む
// - 生成・読み込み先をログ/MessageBoxで可視化

#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <sal.h>
#include <string>
#include <filesystem>

#include "MainUI.hpp"
#include "ConfigManager.h"
#include "DebugManager.h"
#include "RuntimeFlags.hpp"

#pragma comment(lib, "Comctl32.lib")

extern int RunDebugConsoleApp();

namespace {

    std::wstring GetExeDir() noexcept
    {
        wchar_t exePath[MAX_PATH]{};
        DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L"";

        for (int i = (int)n - 1; i >= 0; --i) {
            if (exePath[i] == L'\\' || exePath[i] == L'/') {
                exePath[i] = L'\0';
                break;
            }
        }
        return exePath;
    }

    bool SetCurrentDirToExeDirChecked(std::wstring* outExeDir = nullptr) noexcept
    {
        std::wstring exeDir = GetExeDir();
        if (outExeDir) *outExeDir = exeDir;
        if (exeDir.empty()) return false;

        if (!SetCurrentDirectoryW(exeDir.c_str())) {
            return false;
        }
        return true;
    }

    void InitCommonControlsOnce() noexcept
    {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC =
            ICC_WIN95_CLASSES |
            ICC_TAB_CLASSES |
            ICC_STANDARD_CLASSES |
            ICC_BAR_CLASSES;
        InitCommonControlsEx(&icc);
    }

    bool HasArg(int argc, wchar_t** argv, const wchar_t* key) noexcept
    {
        if (!argv || !key) return false;
        for (int i = 1; i < argc; ++i) {
            if (argv[i] && _wcsicmp(argv[i], key) == 0) return true;
        }
        return false;
    }

    void EnsureConfigIniInExeDir()
    {
        // ★ 先に exeDir へ移動（ここが最重要）
        std::wstring exeDir;
        if (!SetCurrentDirToExeDirChecked(&exeDir)) {
            const DWORD err = GetLastError();
            std::wstring msg = L"[CFG] SetCurrentDirectoryW(exeDir) に失敗しました。err=" + std::to_wstring(err);
            MessageBoxW(nullptr, msg.c_str(), L"TSCEPG", MB_OK | MB_ICONERROR);
            return;
        }

        const std::filesystem::path cfgPath = std::filesystem::path(exeDir) / L"config.ini";
        const bool existedBefore = std::filesystem::exists(cfgPath);

        // ★ ここで初めて ConfigManager を生成する（CWD が exeDir の状態）
        auto& cfg = ConfigManager::GetInstance();

        // 「不足キー補完」＋「ファイルに確実に書く」
        cfg.InitializeConfig();
        cfg.LoadConfig();
        cfg.SaveConfig();
        RuntimeFlags::ApplyAfterConfigSaved();

        const bool existsAfter = std::filesystem::exists(cfgPath);

        // 可視化（必要なら MessageBox を ON に）
        std::wstring info =
            L"ExeDir:\n" + exeDir + L"\n\n" +
            L"ConfigPath:\n" + cfgPath.wstring() + L"\n\n" +
            L"ExistsBefore: " + std::wstring(existedBefore ? L"true" : L"false") + L"\n" +
            L"ExistsAfter : " + std::wstring(existsAfter ? L"true" : L"false") + L"\n";

        DebugLog(L"%s", info.c_str());
        // MessageBoxW(nullptr, info.c_str(), L"TSCEPG Path Info", MB_OK);

        if (!existsAfter) {
            std::wstring msg =
                L"[CFG] config.ini の生成が確認できません。\n\n"
                L"想定出力先:\n" + cfgPath.wstring() + L"\n\n"
                L"exeフォルダに書き込み権限があるかも確認してください。";
            MessageBoxW(nullptr, msg.c_str(), L"TSCEPG", MB_OK | MB_ICONERROR);
        }
    }

} // namespace

int APIENTRY wWinMain(
    _In_ HINSTANCE hInst,
    _In_opt_ HINSTANCE hPrevInst,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInst);
    UNREFERENCED_PARAMETER(lpCmdLine);

    InitCommonControlsOnce();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool wantDebug =
        HasArg(argc, argv, L"--debug") ||
        ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    if (argv) LocalFree(argv);

    // ★ config.ini の生成/読み込み（exeフォルダ固定）
    EnsureConfigIniInExeDir();

    // DebugManager 設定反映
    RuntimeFlags::ApplyAtStartup();

    if (wantDebug) {
        return RunDebugConsoleApp();
    }

    MainUI ui;
    ui.SetActions(GuiActions{});

    if (!ui.CreateAndShow(hInst, nCmdShow)) {
        return 1;
    }

    ui.AppendLog(L"[GUI] TSCEPG GUI started.");
    DebugLog(L"%s", L"[GUI] TSCEPG GUI started.");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    FlushDebugLog();
    return static_cast<int>(msg.wParam);
}
