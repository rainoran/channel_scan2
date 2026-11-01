#pragma once
#ifndef CHANNELLIST_HPP
#define CHANNELLIST_HPP

#include <string>
#include <vector>
#include <cstdint>

#include "ServiceInfo.hpp"
#include "NITNetworkInfo.hpp"

// ================= ソートモード設定 =================
//
// 1: リモコンID順（RemoteControlKeyId → space → channel → serviceId）
// 0: 物理チャンネル順（space → channel → serviceId）
//
// 必要に応じてここの 1/0 を切り替えればビルド時に並び替えモードが変わります。(未完成)
#ifndef CHANNELLIST_SORT_USE_REMOTE_ID
#define CHANNELLIST_SORT_USE_REMOTE_ID 0
#endif

// 1サービス分の統合レコード
struct ChannelListEntry
{
    // 表示用
    std::wstring serviceName;    // SDT: サービス名
    std::wstring providerName;   // SDT: プロバイダ名（現状は空が多い）
    std::wstring networkName;    // NIT: ネットワーク名

    // NIT 由来の TSID/ONID 一覧（デバッグ/解析用）
    std::wstring nitTransportStreams;

    // 物理チャンネル情報（呼び出し側で渡す）
    int space = 0;               // BonDriver space (0: 地デジ等)
    int channel = 0;             // BonDriver ch (物理)

    // ID類
    uint16_t serviceId = 0;          // SDT: service_id
    uint16_t serviceType = 0;        // SDT: service_type
    uint16_t networkId = 0;          // NIT: network_id（≒ ONID）
    uint16_t transportStreamId = 0;  // SDT: TSID
    uint16_t originalNetworkId = 0;  // SDT: ONID

    bool freeCAMode = false;         // SDT: free_CA_mode

    // NIT(TS情報記述子) 由来のリモコンID
    int remoteControlKeyId = -1;     // -1: 未取得
};

// =================== バッファクラス ===================
class ChannelListBuffer
{
public:
    // チャンネルリストへのエントリ追加
    void AddChannelListEntry(
        const std::vector<ServiceInfo>& services,
        const std::vector<NITNetworkInfo>& networks,
        int space,
        int channel);

    // バッファをそのまま保存
    bool SaveChannelListToFile(const std::wstring& filePath);

    // ソート後、一括保存（ソートもここで行う）
    bool SortAndSaveChannelList(const std::wstring& filePath);

private:
    // 一時的にチャンネルを蓄積
    std::vector<ChannelListEntry> allEntries;
};

// SDT / NIT の生リストから統合レコードを作成
std::vector<ChannelListEntry>
BuildChannelListEntries(
    const std::vector<ServiceInfo>& services,
    const std::vector<NITNetworkInfo>& networks,
    int space,
    int channel);



// 追加: 出力ファイル名を一元管理
std::wstring GetChannelListFileName(int space);

// 互換用：引数なし版（デフォルトで地上波 space=0 を返す）
std::wstring GetChannelListFileName();

// 追加: 2回目以降のスキャン時に古いファイルを削除する初期化関数
void InitializeChannelListFile(const std::wstring& filename);

// channellist.ch 形式で追記保存
//  - filePath には通常 L"channellist.ch" を渡す想定
//  - ファイルが存在しない or 空ならヘッダ行を自動出力
bool AppendChannelListFile(
    const std::wstring& filePath,
    const std::vector<ChannelListEntry>& entries,
    bool writeHeaderIfEmpty = true);

#endif // CHANNELLIST_HPP

