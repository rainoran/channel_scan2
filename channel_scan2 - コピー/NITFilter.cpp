#include "NITFilter.hpp"
#include "DebugManager.h"
#include "AribDecoder.hpp"
#include "ConfigManager.h"

#include <algorithm>
#include <string>
#include <iterator>
#include <atomic>
#include <vector>
#include <sstream>
#include <iomanip>

namespace {

    inline decoder::AribDecoder& GetNITAribDecoder() noexcept
    {
        static decoder::AribDecoder s_dec;
        return s_dec;
    }

    static std::wstring SanitizeForLog(std::wstring_view src)
    {
        std::wstring out;
        constexpr std::size_t kMax = 256;
        out.reserve((std::min)(src.size(), kMax));

        for (wchar_t ch : src) {
            if (out.size() >= kMax) { out.push_back(L'…'); break; }
            if (ch == L'\0') { out.append(L"␀"); continue; } // NUL可視化
            if (ch < 0x20) {
                if (ch == L'\t' || ch == L'\n' || ch == L'\r') out.push_back(ch);
                else out.push_back(L'?');
                continue;
            }
            out.push_back(ch);
        }
        return out;
    }

    static std::wstring DumpU16Hex(const std::vector<uint16_t>& v, std::size_t maxCount = 48)
    {
        std::wostringstream oss;
        oss << L"[";
        const std::size_t n = (std::min)(v.size(), maxCount);
        for (std::size_t i = 0; i < n; ++i) {
            if (i) oss << L' ';
            oss << L"0x" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << v[i];
        }
        if (v.size() > maxCount) oss << L" …";
        oss << L"]";
        return oss.str();
    }

    static bool ContainsU16Nul(const std::vector<uint16_t>& v, std::size_t* outPos = nullptr)
    {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i] == 0x0000u) { if (outPos) *outPos = i; return true; }
        }
        return false;
    }

    inline void DecodeAribToU16Codes(const uint8_t* src, std::size_t length, std::vector<uint16_t>& out)
    {
        auto& dec = GetNITAribDecoder();

        // ★重要：static decoder の状態持ち越しを殺す（GUI/CUI差が出やすい）
        dec.BeginString();

        std::u32string u32 = dec.DecodeToU32(src, length);

        out.clear();
        out.reserve(u32.size());
        for (char32_t ch : u32) {
            uint32_t cp = static_cast<uint32_t>(ch);
            out.push_back(cp <= 0xFFFFu ? static_cast<uint16_t>(cp) : 0xFFFDu);
        }
    }

    void ParseNetworkDescriptors(const uint8_t* descPtr, std::size_t descriptors_length, NITNetworkInfo& info)
    {
        std::size_t remain = descriptors_length;

        std::vector<uint16_t> decodedName;
        std::vector<NITDescriptorData> rawList;

        while (remain >= 2) {
            const uint8_t tag = descPtr[0];
            const uint8_t len = descPtr[1];
            if (2u + len > remain) break;

            const uint8_t* body = descPtr + 2;

            NITDescriptorData raw;
            raw.tag = tag;
            raw.data.assign(body, body + len);
            rawList.push_back(std::move(raw));

            // network_name_descriptor
            if (tag == 0x40 && len > 0) {
                DecodeAribToU16Codes(body, len, decodedName);

                // ★観測：NUL混入チェック
                auto& cfg = ConfigManager::GetInstance();
                if (cfg.GetEnableNITDebug()) {
                    std::size_t nulPos = 0;
                    if (ContainsU16Nul(decodedName, &nulPos)) {
                        DebugLog(L"[NIT][WARN] network_name has NUL at pos=%zu len=%zu dump=%s",
                            nulPos, decodedName.size(), DumpU16Hex(decodedName).c_str());
                    }
                    else {
                        // 文字列としても出したい（wchar_t=16bit前提：Windows）
                        std::wstring ws;
                        ws.reserve(decodedName.size());
                        for (auto u : decodedName) ws.push_back(static_cast<wchar_t>(u));

                        DebugLog(L"[NIT] network_name tag=0x40 rawLen=%u decodedLen=%zu name=\"%s\"",
                            (unsigned)len, decodedName.size(), SanitizeForLog(ws).c_str());
                    }
                }
            }

            descPtr += 2 + len;
            remain -= 2 + len;
        }

        if (!decodedName.empty()) info.networkName = std::move(decodedName);

        if (!rawList.empty()) {
            info.networkDescriptors.insert(info.networkDescriptors.end(),
                std::make_move_iterator(rawList.begin()),
                std::make_move_iterator(rawList.end()));
        }
    }

    constexpr uint8_t TS_INFORMATION_DESCRIPTOR_TAG = 0xCD;

    void ParseTransportDescriptors(const uint8_t* descPtr, std::size_t descriptors_length, NITTransportStreamInfo& tsInfo)
    {
        std::size_t remain = descriptors_length;

        while (remain >= 2) {
            const uint8_t tag = descPtr[0];
            const uint8_t len = descPtr[1];
            if (2u + len > remain) break;

            const uint8_t* body = descPtr + 2;

            NITDescriptorData raw;
            raw.tag = tag;
            raw.data.assign(body, body + len);
            tsInfo.descriptors.push_back(std::move(raw));

            // service_list_descriptor
            if (tag == 0x41 && len >= 3) {
                std::size_t off = 0;
                while (off + 3 <= len) {
                    uint16_t serviceId = static_cast<uint16_t>((body[off] << 8) | body[off + 1]);
                    uint8_t  serviceType = body[off + 2];
                    tsInfo.services.push_back(NITServiceListEntry{ serviceId, serviceType });
                    off += 3;
                }
            }

            // TS information descriptor (ARIB)
            if (tag == TS_INFORMATION_DESCRIPTOR_TAG && len >= 1) {
                tsInfo.remoteControlKeyId = static_cast<int>(body[0]);
            }

            descPtr += 2 + len;
            remain -= 2 + len;
        }
    }

} // namespace

NITFilter::NITFilter() = default;

NITFilter::NITFilter(TSParser* parser, PIDDataBuffer* buffer)
    : tsParser(parser), pidBuffer(buffer)
{
}

void NITFilter::Reset() noexcept
{
    sectionBuffer.clear();
    expectedSectionSize = 0;
    haveLastCC = false;
    lastCC = 0xFF;
    networkList.clear();
    recovered = false;
}

bool NITFilter::IsTargetPID(uint16_t pid) const noexcept
{
    return pid == 0x0010;
}

uint32_t NITFilter::ComputePsiCrc32(const uint8_t* data, size_t length) noexcept
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80000000u) crc = (crc << 1) ^ 0x04C11DB7u;
            else crc <<= 1;
        }
    }
    return crc;
}

void NITFilter::FeedBytes(const uint8_t* p, size_t n)
{
    if (!p || n == 0) return;
    sectionBuffer.insert(sectionBuffer.end(), p, p + n);
    TryConsumeSections();
}

void NITFilter::TryConsumeSections()
{
    while (sectionBuffer.size() >= 3) {
        if (expectedSectionSize == 0) {
            const size_t sl = ((sectionBuffer[1] & 0x0F) << 8) | sectionBuffer[2];
            if (sl < 5 || sl > 1021) {
                sectionBuffer.erase(sectionBuffer.begin());
                continue;
            }
            expectedSectionSize = 3 + sl;
        }

        if (sectionBuffer.size() < expectedSectionSize) break;

        InputSection(sectionBuffer.data(), expectedSectionSize);

        sectionBuffer.erase(sectionBuffer.begin(), sectionBuffer.begin() + expectedSectionSize);
        expectedSectionSize = 0;
    }
}

void NITFilter::InputPacket(const uint8_t* data, std::size_t size)
{
    if (!data || size < 188) return;
    if (data[0] != 0x47) return;

    const uint16_t pid = static_cast<uint16_t>(((data[1] & 0x1F) << 8) | data[2]);
    if (pid != 0x0010) return;

    const uint8_t afc = (data[3] >> 4) & 0x03;
    if ((afc & 0x01) == 0) return;

    const uint8_t cc = data[3] & 0x0F;
    if (haveLastCC) {
        const uint8_t expect = static_cast<uint8_t>((lastCC + 1) & 0x0F);
        if (cc != expect) {
            sectionBuffer.clear();
            expectedSectionSize = 0;

            auto& cfg = ConfigManager::GetInstance();
            if (cfg.GetEnableNITDebug()) {
                DebugLog(L"[NIT][WARN] CC discontinuity: last=%u now=%u -> buffer reset", (unsigned)lastCC, (unsigned)cc);
            }
        }
    }
    haveLastCC = true;
    lastCC = cc;

    size_t p = 4;
    if (afc & 0x02) {
        if (p >= size) return;
        const uint8_t adapt_len = data[p];
        p += 1 + adapt_len;
        if (p >= size) return;
    }

    const bool pusi = (data[1] & 0x40) != 0;

    if (pusi) {
        if (p >= size) return;
        const uint8_t pointer = data[p++];
        if (p + pointer > size) return;

        if (pointer > 0 && !sectionBuffer.empty()) {
            FeedBytes(data + p, pointer);
        }
        p += pointer;

        if (p < size) {
            FeedBytes(data + p, size - p);
        }
    }
    else {
        if (p < size) {
            FeedBytes(data + p, size - p);
        }
    }
}

void NITFilter::InputSection(const uint8_t* section, std::size_t length)
{
    if (!section || length < 12) return;

    const uint8_t table_id = section[0];
    if (table_id != 0x40 && table_id != 0x41) return;

    if (ComputePsiCrc32(section, length) != 0) {
        auto& cfg = ConfigManager::GetInstance();
        if (cfg.GetEnableNITDebug()) {
            DebugLog(L"[NIT][WARN] CRC32 mismatch. length=%zu table_id=0x%02X", length, (unsigned)table_id);
        }
        return;
    }

    const uint16_t section_length = static_cast<uint16_t>(((section[1] & 0x0F) << 8) | section[2]);
    if (3u + section_length != length) return;

    const uint16_t network_id = static_cast<uint16_t>((section[3] << 8) | section[4]);

    const uint16_t network_descriptors_length =
        static_cast<uint16_t>(((section[8] & 0x0F) << 8) | section[9]);

    const std::size_t header_size = 10;
    if (header_size + network_descriptors_length + 2 > length) return;

    const uint8_t* p = section + header_size;
    const uint8_t* end = section + (3u + section_length - 4u);

    NITNetworkInfo parsed;
    parsed.networkId = network_id;

    if (network_descriptors_length > 0) {
        if (p + network_descriptors_length > end) return;
        ParseNetworkDescriptors(p, network_descriptors_length, parsed);
        p += network_descriptors_length;
    }

    if (p + 2 > end) return;

    const uint16_t ts_loop_length =
        static_cast<uint16_t>(((p[0] & 0x0F) << 8) | p[1]);
    p += 2;
    if (p + ts_loop_length > end) return;

    const uint8_t* tsLoopEnd = p + ts_loop_length;

    while (p + 6 <= tsLoopEnd) {
        NITTransportStreamInfo tsInfo;
        tsInfo.transportStreamId = static_cast<uint16_t>((p[0] << 8) | p[1]);
        tsInfo.originalNetworkId = static_cast<uint16_t>((p[2] << 8) | p[3]);

        const uint16_t desc_len = static_cast<uint16_t>(((p[4] & 0x0F) << 8) | p[5]);
        p += 6;
        if (p + desc_len > tsLoopEnd) break;

        if (desc_len > 0) {
            ParseTransportDescriptors(p, desc_len, tsInfo);
        }
        p += desc_len;

        parsed.transportStreamIds.push_back(tsInfo.transportStreamId);
        parsed.transportStreams.push_back(std::move(tsInfo));
    }

    bool merged = false;
    for (auto& n : networkList) {
        if (n.networkId == parsed.networkId) {
            if (n.networkName.empty() && !parsed.networkName.empty()) {
                n.networkName = std::move(parsed.networkName);
            }
            if (!parsed.networkDescriptors.empty()) {
                n.networkDescriptors.insert(n.networkDescriptors.end(),
                    std::make_move_iterator(parsed.networkDescriptors.begin()),
                    std::make_move_iterator(parsed.networkDescriptors.end()));
            }

            for (auto& ts : parsed.transportStreams) {
                auto it = std::find_if(n.transportStreams.begin(), n.transportStreams.end(),
                    [&](const NITTransportStreamInfo& cur) {
                        return cur.transportStreamId == ts.transportStreamId &&
                            cur.originalNetworkId == ts.originalNetworkId;
                    });
                if (it == n.transportStreams.end()) {
                    n.transportStreamIds.push_back(ts.transportStreamId);
                    n.transportStreams.push_back(std::move(ts));
                }
            }
            merged = true;
            break;
        }
    }
    if (!merged) networkList.push_back(std::move(parsed));

    recovered = true;

    auto& cfg = ConfigManager::GetInstance();
    if (cfg.GetEnableNITDebug()) {
        DebugLog(L"[NIT] recovered=true network_id=%u networks=%zu",
            (unsigned)network_id, networkList.size());
    }
}
