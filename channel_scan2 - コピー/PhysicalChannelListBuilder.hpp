#pragma once
#ifndef PHYSICAL_CHANNEL_LIST_BUILDER_HPP
#define PHYSICAL_CHANNEL_LIST_BUILDER_HPP

#include <vector>
#include "ChannelListLoader.hpp"

// チャンネルリスト（サービス単位）から、EIT などで使う
// 一時的な「物理チャンネル一覧」を構築する専用ユニット。
class PhysicalChannelListBuilder
{
public:
    // 1物理チャンネル = space + channel + そのチャンネルに載っているサービス一覧
    struct PhysicalChannel
    {
        int space = 0;
        int channel = 0;

        // この物理チャンネルに含まれるサービス
        //   ※元の ChannelEntry へのポインタを持つだけで、書き換えはしない
        std::vector<const ChannelListLoader::ChannelEntry*> services;
    };

    // 全 space を対象に「物理チャンネル一覧」を構築
    static std::vector<PhysicalChannel>
        BuildAll(const std::vector<ChannelListLoader::ChannelEntry>& entries);

    // 特定 space だけを対象に「物理チャンネル一覧」を構築
    static std::vector<PhysicalChannel>
        BuildForSpace(const std::vector<ChannelListLoader::ChannelEntry>& entries, int space);
};

#endif // PHYSICAL_CHANNEL_LIST_BUILDER_HPP
