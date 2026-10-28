#pragma once
#ifndef TSBUFFERSTATUS_HPP
#define TSBUFFERSTATUS_HPP

//=================================================================================
//  TSBufferStatus.hpp
//  概要:
//      TSBuffer クラスなどで使用されるステータス列挙型。
//      リングバッファ操作の成否や状態を明確に示すために使用します。
//=================================================================================

/**
 * @brief バッファ操作の結果を示すステータス列挙型
 */
enum class TSBufferStatus {
    Success,       ///< 処理成功
    OverFlow,      ///< バッファオーバーフロー（空き容量なし）
    UnderFlow,     ///< バッファアンダーフロー（データ不足）
    Error          ///< その他のエラー（例: 無効なポインタなど）
};

#endif // TSBUFFERSTATUS_HPP