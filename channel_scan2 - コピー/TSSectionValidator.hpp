#pragma once
#ifndef TSSECTIONVALIDATOR_HPP
#define TSSECTIONVALIDATOR_HPP

#include <cstdint>
#include <cstddef>
#include <iostream>
#include "DebugManager.h"


namespace TSValidator {
    // TS ペイロード（pointer_field 付き）用
    bool IsValidSection(const uint8_t* payload, size_t payloadSize);

    // 生セクション（table_id から始まる配列）用
    bool IsValidRawSection(const uint8_t* section, size_t length);

    // 統計制御
    void ResetStats() noexcept;
    void DumpStatsToDebugLog();

    // EIT 用の「生セクションヘッダ」検証
    //  - 引数 section は table_id から始まる「完成済みセクション」
    //  - length は section 全体のバイト数（CRC込み）
    //  - EIT 以外の table_id の場合は false を返す
    bool IsValidEITSectionHeader(const uint8_t* section, size_t length);
}

#endif // TSSECTIONVALIDATOR_HPP
