#include "ts_receiver_test.hpp"
#include "ts_receiver.hpp"
#include "BonDriverHelper.hpp" // ← BonDriverHelper をインクルード
#include "DebugManager.h"
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include "TSStreamUtils.hpp"

void RunTsStreamTest(IBonDriver2* bon)
{
    if (!bon)
    {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    DebugLog(L"[INFO] TSストリーム受信を開始します.\n");

    // チャンネル選択・信号ロック・受信テスト
    const int space = 0;
    int ch = 0;
    if (!FindAndSetValidChannel_Terrestrial(bon, ch)) {
        DebugLog(L"[ERROR] 有効なチャンネルが見つかりません。\n");
        return;
    }

    DebugLogFormat(L"[INFO] 選択されたチャンネル: space=%d, ch=%d\n", space, ch);

    TSStreamUtils::WaitForSignalLock(bon);  // 信号安定化チェック
    Sleep(1000);             // ロック安定待ち

    TsReceiver receiver;
    receiver.Start(bon, false, L"ts_test_output.ts");
    Sleep(3000);
    receiver.Stop();

    // バッファにデータがあるか確認
    std::vector<uint8_t> data;
    if (receiver.GetBufferedTSStream(data))
    {
        DebugLogFormat(L"[OK] TSデータ取得完了 - 総サイズ: %zu bytes\n", data.size());
        TSStreamUtils::PrintHex(data.data(), std::min<size_t>(data.size(), 188)); // ダンプ出力
    }
    else
    {
        DebugLog(L"[ERROR] TSデータが取得できませんでした\n");
    }

    

    
}

