#pragma once
#ifndef NITNETWORKINFO_HPP
#define NITNETWORKINFO_HPP

#include <cstdint>
#include <vector>

// NIT 内の descriptor 生データ
struct NITDescriptorData
{
    uint8_t tag = 0;
    std::vector<uint8_t> data;
};

// service_list_descriptor 等から得られるサービス情報
struct NITServiceListEntry
{
    uint16_t serviceId  = 0;
    uint8_t  serviceType = 0;
};

// 1つの TS (transport stream) の情報
struct NITTransportStreamInfo
{
    uint16_t transportStreamId = 0; // TSID
    uint16_t originalNetworkId = 0; // ONID

    // service_list_descriptor 由来
    std::vector<NITServiceListEntry> services;

    // TS 情報記述子などから得られる追加情報（将来用）
    int remoteControlKeyId = -1;          // -1: 未取得
    std::vector<uint16_t> tsName;         // TS 名 (ARIB デコード結果を UTF-16 コードとして)

    // 当該 TS に対して NIT の TS ループ内に出現した descriptor 全ての RAW
    std::vector<NITDescriptorData> descriptors;
};

// 1つの network_id に対応する情報
struct NITNetworkInfo
{
    // NIT の network_id
    uint16_t networkId = 0;

    // 旧来フィールド（互換用）：TSID のリスト
    std::vector<uint16_t> transportStreamIds;

    // 拡張版 TS 情報 (TSID + ONID + descriptor 群など)
    std::vector<NITTransportStreamInfo> transportStreams;

    // デコード済みネットワーク名 (UTF-16 コード単位)
    std::vector<uint16_t> networkName;

    // network_descriptors_loop() に出現した descriptor の RAW
    std::vector<NITDescriptorData> networkDescriptors;
};

#endif // NITNETWORKINFO_HPP
