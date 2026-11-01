#pragma once
#ifndef SIMDSELECT_H
#define SIMDSELECT_H

#include <string>


// **拡張命令セットの選択関数**
std::string SelectBestSIMD();

// **実行時の関数ポインタ**
extern void (*SIMDfunction)(void);

// **SCALAR処理用の関数を extern 宣言**
void SIMD_SCALAR();

#endif // SIMDSELECT_H

