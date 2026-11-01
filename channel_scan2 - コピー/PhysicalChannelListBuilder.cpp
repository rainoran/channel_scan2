#include "PhysicalChannelListBuilder.hpp"

#include <map>

namespace {

    struct PhysicalKey
    {
        int space;
        int channel;

        bool operator<(const PhysicalKey& rhs) const noexcept
        {
            if (space != rhs.space)
                return space < rhs.space;
            return channel < rhs.channel;
        }
    };

} // namespace

// 全 space 対象
std::vector<PhysicalChannelListBuilder::PhysicalChannel>
PhysicalChannelListBuilder::BuildAll(
    const std::vector<ChannelListLoader::ChannelEntry>& entries)
{
    std::map<PhysicalKey, PhysicalChannel> map;

    for (const auto& e : entries) {
        PhysicalKey key{ e.space, e.channel };
        auto& pc = map[key];

        if (pc.services.empty()) {
            pc.space = e.space;
            pc.channel = e.channel;
        }

        pc.services.push_back(&e);
    }

    std::vector<PhysicalChannel> result;
    result.reserve(map.size());

    for (auto& kv : map) {
        result.push_back(std::move(kv.second));
    }

    return result;
}

// 特定 space だけ対象
std::vector<PhysicalChannelListBuilder::PhysicalChannel>
PhysicalChannelListBuilder::BuildForSpace(
    const std::vector<ChannelListLoader::ChannelEntry>& entries,
    int space)
{
    std::map<PhysicalKey, PhysicalChannel> map;

    for (const auto& e : entries) {
        if (e.space != space)
            continue;

        PhysicalKey key{ e.space, e.channel };
        auto& pc = map[key];

        if (pc.services.empty()) {
            pc.space = e.space;
            pc.channel = e.channel;
        }

        pc.services.push_back(&e);
    }

    std::vector<PhysicalChannel> result;
    result.reserve(map.size());

    for (auto& kv : map) {
        result.push_back(std::move(kv.second));
    }

    return result;
}
