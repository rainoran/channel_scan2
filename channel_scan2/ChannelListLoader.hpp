#pragma once
#ifndef CHANNEL_LIST_LOADER_H
#define CHANNEL_LIST_LOADER_H

#include <string>
#include <vector>


class ChannelListLoader {
public:
    struct ChannelEntry
    {
        // 0: ServiceName
        std::wstring serviceName;

        // 1: Space / 2: Channel
        int space = 0;
        int channel = 0;

        // 3: ServiceType / 4: ServiceID
        uint16_t serviceType = 0;
        uint16_t serviceId = 0;

        // 5: NetworkID / 6: TSID / 7: ONID
        uint16_t networkId = 0;
        uint16_t tsid = 0;
        uint16_t onid = 0;

        // 8: FreeCA (0 or 1)
        bool freeCA = false;

        // 9: Provider / 10: NetworkName
        std::wstring provider;
        std::wstring networkName;

        // 11: RemoteControlKeyID
        int remoteControlKeyId = -1;
    };

    // INIファイルを読み込む or 生成
    bool LoadFromINI(const std::wstring& filename);

    // channellist_spaceX.ch から直接読み込む
    bool LoadFromChannelListCh(const std::wstring& filename);

    // チャンネルリスト取得
    const std::vector<ChannelEntry>& GetChannelList() const noexcept { return m_channels; }


	// チャンネルリストクリア
    void Clear() noexcept { m_channels.clear(); }

private:
    std::vector<ChannelEntry> channelList;
    std::vector<ChannelEntry> m_channels;

    // 既存INI読み込み
    bool LoadExistingINI(const std::wstring& filename);

    // TXT→INI変換
    bool ConvertFromTXT(const std::wstring& txtPath, const std::wstring& iniPath);

    // 初期INIを作成する
    bool CreateInitialINI(const std::wstring& filename);

    // トリム処理
    std::wstring Trim(const std::wstring& str);

    // 文字列 → uint16_t（安全変換）
    uint16_t ToUInt16Safe(const std::wstring& str);
};

#endif // CHANNEL_LIST_LOADER_H
