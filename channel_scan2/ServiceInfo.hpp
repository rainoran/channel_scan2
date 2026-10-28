#pragma once
#ifndef SERVICE_INFO_HPP
#define SERVICE_INFO_HPP

#include <string>
#include <cstdint> 

// SDTのサービス情報構造体
struct ServiceInfo
{
    // --- 既存フィールド ---
    uint16_t    service_id = 0;
    std::wstring service_name;
    uint8_t     service_type = 0;
    uint8_t     section_number = 0;
    bool        free_CA_mode = false;
    std::wstring provider_name; // ← こちらもARIB

    // --- 追加フィールド ---

    // SDTセクションヘッダから取れる識別子
    uint16_t    transport_stream_id = 0;   // TSID (data[3..4])
    uint16_t    original_network_id = 0;   // ONID (data[8..9])

    // サービスループ内のフラグ類
    bool        eit_schedule_flag = false;          // EIT_schedule_flag
    bool        eit_present_following_flag = false; // EIT_present_following_flag
    uint8_t     running_status = 0;                 // running_status (0=未定義,4=本運用など)
};
#endif // SERVICE_INFO_HPP
