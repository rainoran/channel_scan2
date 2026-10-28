// main.cpp (Debug Menu Unit)  C++20
// - GUIから呼ばれたら新規コンソールを作ってデバッグメニューを実行する
// - エントリポイント(main/wmain/wWinMain)は置かない

#define NOMINMAX
#include <Windows.h>
#include <io.h>
#include <fcntl.h>

#include <iostream>
#include <vector>
#include <filesystem>
#include <limits>
#include <locale>

#include "bondriver_scan.h"
#include "bondriver_load.h"
#include "channel_scan.h"
#include "configmanager.h"
#include "SIMDselect.h"
#include "MathOps.h"
#include "DebugManager.h"
#include "ts_receiver_test.hpp"
#include "ts_filter_test.hpp"
#include "ChannelListLoader.hpp"
#include "MPMCRingBufferTest.hpp"
#include "FilterManager.hpp"

namespace {

    void CreateSeparateConsoleWindow() noexcept
    {
        // 既存コンソールがあっても「別ウィンドウ」が欲しいので一旦切る
        FreeConsole();

        if (!AllocConsole()) {
            // 失敗しても落とさない
            return;
        }

        // stdio を新規コンソールへ接続
        FILE* fp = nullptr;
        freopen_s(&fp, "CONIN$", "r", stdin);
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);

        // CRTをUTF-16テキスト扱いに（wcin/wcout 安定用）
        _setmode(_fileno(stdin), _O_U16TEXT);
        _setmode(_fileno(stdout), _O_U16TEXT);
        _setmode(_fileno(stderr), _O_U16TEXT);

        std::ios::sync_with_stdio(true);

        // コンソールタイトル
        SetConsoleTitleW(L"TSCEPG Debug Console");
    }

    void ApplyConsoleEncodingFromConfig(ConfigManager& cfg) noexcept
    {
        // 既存の運用：ConfigManager::SetLogEncoding(L"utf8"/L"sjis") を使っている前提
        // GetLogEncoding() が無い場合でも落とさない
        std::wstring enc = L"sjis";
        try {
            enc = cfg.GetLogEncoding();
        }
        catch (...) {}

        try {
            if (enc == L"utf8") {
                std::locale::global(std::locale(".UTF-8"));
                std::wcout.imbue(std::locale());
                std::wcin.imbue(std::locale());
                SetConsoleOutputCP(CP_UTF8);
                SetConsoleCP(CP_UTF8);
            }
            else {
                std::locale::global(std::locale("Japanese_Japan.932"));
                std::wcout.imbue(std::locale());
                std::wcin.imbue(std::locale());
                SetConsoleOutputCP(932);
                SetConsoleCP(932);
            }
        }
        catch (...) {
            // ロケール設定に失敗しても継続
            std::locale::global(std::locale::classic());
            std::wcout.imbue(std::locale());
            std::wcin.imbue(std::locale());
        }
    }

    int RunMenu(IBonDriver2* bonDriver2)
    {
        if (!bonDriver2) {
            std::wcout << L"[ERROR] BonDriver が無効です。\n";
            return 1;
        }

        while (true) {
            std::wcout << L"=== デバッグメニュー ===\n";
            std::wcout << L"1: チャンネルスキャン\n";
            std::wcout << L"2: TS受信テスト\n";
            std::wcout << L"3: MPMCリングバッファテスト\n";
            std::wcout << L"4: PIDフィルター処理テスト\n";
            std::wcout << L"5: フィルター処理テスト(SDT+NIT)\n";
            std::wcout << L"6: 番組表取得テスト\n";
            std::wcout << L"7: チャンネルリストからの番組表取得テスト\n";
            std::wcout << L"8: EPG ダンプパースチェック\n";
            std::wcout << L"9: EPG HTML 生成テスト\n";
            std::wcout << L"10: EPG ソートデバッグテスト\n";
            std::wcout << L"11: 番組表構築テスト\n";
            std::wcout << L"13: 番組表構築テスト+更新機能テスト\n";
            std::wcout << L"14: EIT取得 → HTML EPG生成テスト\n";
            std::wcout << L"19: TDT/TOT 受信テスト\n";
            std::wcout << L"22: CDTフィルタ動作テスト\n";
            std::wcout << L"23: CAT/BIT フィルタ動作テスト\n";
            std::wcout << L"24: RSTフィルタ動作テスト\n";
            std::wcout << L"25: TSDT/DIT/SITフィルタ動作テスト\n";
            std::wcout << L"0: 終了\n";
            std::wcout << L"選択: ";

            int choice = -1;
            if (!(std::wcin >> choice)) {
                std::wcin.clear();
                std::wcin.ignore((std::numeric_limits<std::streamsize>::max)(), L'\n');
                std::wcout << L"[WARN] 入力が不正です。\n";
                continue;
            }

            FilterManagerMode fmMode = FilterManagerMode::ChannelScan;

            switch (choice) {
            case 1:
                std::wcout << L"[INFO] チャンネルスキャンを実行します。\n";
                ScanChannels(bonDriver2, L"channels.txt");
                break;

            case 2:
                std::wcout << L"[INFO] TS受信テストを開始します。\n";
                RunTsStreamTest(bonDriver2);
                break;

            case 3:
                std::wcout << L"[INFO] MPMCリングバッファテストを実行します。\n";
                RunMPMCRingBufferTest(bonDriver2);
                break;

            case 4:
                std::wcout << L"[INFO] PIDフィルター処理テストを実行します。\n";
                RunMPMCRingBufferWithPIDTest(bonDriver2);
                break;

            case 5:
                std::wcout << L"[INFO] フィルター処理テスト（ChannelScan）を実行します。\n";
                fmMode = FilterManagerMode::ChannelScan;
                RunFilterProcessingTest(bonDriver2, fmMode);
                break;

            case 6:
                std::wcout << L"[INFO] 番組表取得テスト（EpgFullScan）を実行します。\n";
                fmMode = FilterManagerMode::EpgFullScan;
                RunEpgDataTest(bonDriver2, fmMode);
                break;

            case 7:
                std::wcout << L"[INFO] チャンネルリストからの番組表取得テストを実行します。\n";
                fmMode = FilterManagerMode::EpgFullScan;
                RunEpgDataTest_FromChannelList(bonDriver2, fmMode, /*space=*/0);
                break;

            case 8:
                std::wcout << L"[INFO] EPG ダンプパースチェックを実行します。\n";
                RunEpgDumpParseCheck();
                break;

            case 9:
                std::wcout << L"[INFO] EPG HTML 生成テストを実行します。\n";
                RunEpgHtmlTest();
                break;

            case 10:
                std::wcout << L"[INFO] EPG ソートデバッグテストを実行します。\n";
                RunEpgSortDebugTest();
                break;

            case 11:
                std::wcout << L"[INFO] 番組表構築テストを実行します。\n";
                RunEpgBuildTest(bonDriver2, /*enableUpdate=*/false);
                break;

            case 13:
                std::wcout << L"[INFO] 番組表構築テスト+更新機能テストを実行します。\n";
                RunEpgBuildTest(bonDriver2, /*enableUpdate=*/true);
                break;

            case 14:
                std::wcout << L"[INFO] EIT取得 → HTML EPG生成（RunEpgBuildTest2）を実行します。\n";
                RunEpgBuildTest2(bonDriver2, FilterManagerMode::EpgFullScan, /*enableUpdate*/ false);
                break;

            case 19:
                std::wcout << L"[INFO] TDT/TOT テストを実行します。\n";
                RunTdtTotTest(bonDriver2);
                break;

            case 22:
                std::wcout << L"[INFO] CDTフィルタ動作テストを実行します。\n";
                RunCdtTest(bonDriver2, /*space=*/0, /*captureSec=*/10);
                break;

            case 23:
                std::wcout << L"[INFO] CAT/BITフィルタ動作テストを実行します。\n";
                RunCatBitTest(bonDriver2, /*space=*/0, /*captureSec=*/10);
                break;

            case 24:
                std::wcout << L"[INFO] RSTフィルタ動作テストを実行します。\n";
                RunRstTest(bonDriver2, /*space=*/0, /*captureSec=*/10);
                break;

            case 25:
                std::wcout << L"[INFO] TSDT/DIT/SITフィルタ動作テストを実行します。\n";
                RunTsdtDitSitScanTest(bonDriver2);
                break;

            case 0:
                std::wcout << L"終了します。\n";
                FlushDebugLog();
                return 0;

            default:
                std::wcout << L"未対応の選択です。\n";
                break;
            }

            FlushDebugLog();

            std::wcout << L"\n続けるには Enter キーを押してください...\n";
            std::wcin.ignore((std::numeric_limits<std::streamsize>::max)(), L'\n');
            std::wcin.get();
        }
    }

} // namespace

// ------------------------------------------------------------
// GUI側から呼ばれるエントリ（ここだけ外部公開）
// ------------------------------------------------------------
int RunDebugConsoleApp()
{
    // ★ GUIとは別のコンソールウィンドウを確実に出す
    CreateSeparateConsoleWindow();

    ConfigManager& config = ConfigManager::GetInstance();
    if (!std::filesystem::exists("config.ini")) {
        config.InitializeConfig();
    }

    ApplyConsoleEncodingFromConfig(config);

    // DebugManager 設定反映（ファイル出力ON/OFFなど）
    DebugManager::GetInstance().ConfigureDebugMode();

    std::wcout << L"[INFO] Debug console initialized.\n";
    DebugLog(L"[INFO] DebugLog routed to this console."); // WriteConsoleW 経由 :contentReference[oaicite:1]{index=1}

    // BonDriver 設定
    std::wstring bonDriverPath = config.GetBonDriverPath();
    if (bonDriverPath.empty()) {
        std::wcout << L"[INFO] BonDriver 未設定。スキャンします。\n";
        std::vector<std::wstring> bonDrivers = ScanBonDriverFiles(L".");
        if (bonDrivers.empty()) {
            std::wcout << L"[ERROR] BonDriver が見つかりません。\n";
            return 1;
        }

        bonDriverPath = SelectBonDriver(bonDrivers);
        if (bonDriverPath.empty()) {
            std::wcout << L"[ERROR] BonDriver 選択がキャンセルされました。\n";
            return 1;
        }

        config.SetBonDriverPath(bonDriverPath);
        config.SaveConfig();
    }

    // SIMD 初期化（従来通り）
    InitMathOps(SelectBestSIMD());
    SIMDfunction();

    // BonDriver ロード
    BonDriverLoader loader;
    if (!loader.LoadBonDriver(bonDriverPath)) {
        std::wcout << L"[ERROR] BonDriver ロード失敗。\n";
        return 1;
    }
    IBonDriver2* bonDriver2 = loader.GetBonDriver();
    if (!bonDriver2) {
        std::wcout << L"[ERROR] IBonDriver2 取得失敗。\n";
        return 1;
    }

    std::wcout << L"[INFO] BonDriver loaded and ready.\n";
    const int ret = RunMenu(bonDriver2);

    FlushDebugLog();
    // 終了時にコンソールを閉じたいなら FreeConsole();（任意）
    return ret;
}
