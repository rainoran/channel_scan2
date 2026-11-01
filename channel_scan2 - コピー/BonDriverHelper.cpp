#include "BonDriverHelper.hpp"
#include "DebugManager.h"

int FindAndSetValidChannel_Terrestrial(IBonDriver2* bon, int space) {
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver インスタンスが NULL です。");
        return -1;
    }

    const DWORD maxChannel = 50; // 地上波は最大 ch15 まで (0〜15)

    for (DWORD ch = 0; ch < maxChannel; ++ch) {
        if (bon->SetChannel(space, ch)) {
            DebugLog(L"[INFO] 有効なチャンネルを検出: space=%u, ch=%u", space, ch);
            return static_cast<int>(ch);
        }
    }

    DebugLog(L"[WARN] 有効なチャンネルが見つかりませんでした (space=%d)", space);
    return -1;
}
