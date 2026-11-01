#ifndef BONDRIVER_HELPER_HPP
#define BONDRIVER_HELPER_HPP

#include <windows.h>
#include "IBonDriver2.h"

// 有効なチャンネルを検索してチャンネル設定を行う。成功時はチャンネル番号を返し、失敗時は -1 を返す。
int FindAndSetValidChannel_Terrestrial(IBonDriver2* bon, int space = 0);

#endif // BONDRIVER_HELPER_HPP
