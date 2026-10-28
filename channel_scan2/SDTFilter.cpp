#include "SDTFilter.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <iomanip>

#include "PacketDumpManager.hpp"
#include "ConfigManager.h"

namespace {

    static std::wstring SanitizeForLog(std::wstring_view src)
    {
        std::wstring out;
        constexpr std::size_t kMax = 256;
        out.reserve((std::min)(src.size(), kMax));

        for (wchar_t ch : src) {
            if (out.size() >= kMax) { out.push_back(L'…'); break; }
            if (ch == L'\0') { out.append(L"␀"); continue; }
            if (ch < 0x20) {
                if (ch == L'\t' || ch == L'\n' || ch == L'\r') out.push_back(ch);
                else out.push_back(L'?');
                continue;
            }
            out.push_back(ch);
        }
        return out;
    }

    static std::wstring DumpWStringHex(std::wstring_view s, std::size_t maxCount = 48)
    {
        std::wostringstream oss;
        oss << L"[";
        const std::size_t n = (std::min)(s.size(), maxCount);
        for (std::size_t i = 0; i < n; ++i) {
            if (i) oss << L' ';
            oss << L"0x" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0')
                << (static_cast<unsigned>(static_cast<uint16_t>(s[i])));
        }
        if (s.size() > maxCount) oss << L" …";
        oss << L"]";
        return oss.str();
    }

    static bool ContainsNul(std::wstring_view s, std::size_t* outPos = nullptr)
    {
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'\0') { if (outPos) *outPos = i; return true; }
        }
        return false;
    }

} // namespace

SDTFilter::SDTFilter(TSParser* p, PIDDataBuffer* b)
    : parser(p), pidBuffer(b)
{
}

bool SDTFilter::IsTargetPID(uint16_t pid) const
{
    return pid == 0x0011;
}

void SDTFilter::InputPacket(const std::array<uint8_t, 188>& pkt)
{
    const uint8_t* data = pkt.data();

    uint8_t afc = (data[3] >> 4) & 0x03;
    if (afc == 0x02) return;
    size_t payload_start = 4;
    if (afc == 0x03) {
        if (payload_start >= 188) return;
        const size_t af_len = data[payload_start];
        if (payload_start + 1 + af_len > 188) return;
        payload_start += 1 + af_len;
    }
    if (payload_start >= 188) return;

    const bool pusi = (data[1] & 0x40) != 0;
    const uint8_t* payload = data + payload_start;
    size_t payload_size = 188 - payload_start;
    if (payload_size == 0) return;

    sdtRawPacketsAfter.push_back(pkt);

    if (!pusi) {
        sectionBuffer.insert(sectionBuffer.end(), payload, payload + payload_size);
    }
    else {
        if (payload_size < 1) return;
        const std::size_t pointer_field = static_cast<std::size_t>(payload[0]);
        if (1 + pointer_field > payload_size) return;

        if (pointer_field > 0 && !sectionBuffer.empty()) {
            sectionBuffer.insert(sectionBuffer.end(), payload + 1, payload + 1 + pointer_field);
        }

        while (sectionBuffer.size() >= 3) {
            // セクション長は、ビット演算前に size_t へ拡張する。
            const std::size_t sec_len =
                (static_cast<std::size_t>(sectionBuffer[1] & 0x0Fu) << 8u) |
                static_cast<std::size_t>(sectionBuffer[2]);
            if (sec_len < 9 || sec_len > 1021) {
                sectionBuffer.erase(sectionBuffer.begin());
                continue;
            }
            const size_t total = 3 + sec_len;
            if (sectionBuffer.size() < total) break;

            ParseSDT(sectionBuffer.data(), total, pkt);
            sectionBuffer.erase(sectionBuffer.begin(), sectionBuffer.begin() + total);
        }

        sectionBuffer.clear();
        const uint8_t* sec_start = payload + 1 + pointer_field;
        const size_t   sec_bytes = payload_size - 1 - pointer_field;
        if (sec_bytes > 0) {
            sectionBuffer.insert(sectionBuffer.end(), sec_start, sec_start + sec_bytes);
        }
    }

    while (sectionBuffer.size() >= 3) {
        // セクション長は、ビット演算前に size_t へ拡張する。
        const std::size_t sec_len =
            (static_cast<std::size_t>(sectionBuffer[1] & 0x0Fu) << 8u) |
            static_cast<std::size_t>(sectionBuffer[2]);
        if (sec_len < 9 || sec_len > 1021) {
            sectionBuffer.erase(sectionBuffer.begin());
            continue;
        }
        const size_t total = 3 + sec_len;
        if (sectionBuffer.size() < total) break;

        ParseSDT(sectionBuffer.data(), total, pkt);
        sectionBuffer.erase(sectionBuffer.begin(), sectionBuffer.begin() + total);
    }
}

bool SDTFilter::IsRecovered() const
{
    return recovered;
}

void SDTFilter::ParseSDT(const uint8_t* data, size_t size, const std::array<uint8_t, 188>& /*pkt*/)
{
    if (!data || size < 11) return;
    const uint8_t table_id = data[0];
    if (table_id != 0x42 && table_id != 0x46) return;

    const uint16_t section_length = static_cast<uint16_t>(((data[1] & 0x0F) << 8) | data[2]);
    if (std::size_t{ 3 } + static_cast<std::size_t>(section_length) != size) return;

    const uint8_t section_number = data[6];
    const uint8_t last_section_number = data[7];

    lastSectionNumber = std::max<int>(lastSectionNumber, static_cast<int>(last_section_number));
    receivedSections.insert(section_number);
    if (lastSectionNumber >= 0 && receivedSections.size() >= static_cast<size_t>(lastSectionNumber + 1)) {
        recovered = true;
    }

    const uint16_t transport_stream_id = static_cast<uint16_t>((data[3] << 8) | data[4]);
    const uint16_t original_network_id = static_cast<uint16_t>((data[8] << 8) | data[9]);

    const uint8_t* loop_start = data + 11;
    const uint8_t* loop_end = data + 3 + section_length - 4;

    decoder::AribDecoder decoder; // SDT側は毎回ローカルでOK

    auto& cfg = ConfigManager::GetInstance();
    const bool dbg = cfg.GetEnableSDTDebug();

    while (loop_start + 5 <= loop_end) {
        const uint8_t* loop = loop_start;

        const uint16_t service_id = static_cast<uint16_t>((loop[0] << 8) | loop[1]);
        const bool eit_schedule_flag = (loop[2] & 0x02) != 0;
        const bool eit_present_following_flag = (loop[2] & 0x01) != 0;
        const uint8_t running_status = static_cast<uint8_t>((loop[3] & 0xE0) >> 5);
        const bool free_CA_mode = (loop[3] & 0x10) != 0;

        const uint16_t descriptors_loop_length = static_cast<uint16_t>(((loop[3] & 0x0F) << 8) | loop[4]);

        const uint8_t* desc = loop + 5;
        const uint8_t* desc_end = desc + descriptors_loop_length;
        if (desc_end > loop_end) break;

        std::wstring service_name;
        std::wstring provider_name;
        uint8_t service_type = 0;

        while (desc + 2 <= desc_end) {
            const uint8_t tag = desc[0];
            const uint8_t len = desc[1];
            if (desc + 2 + len > desc_end) break;

            if (tag == 0x48 && len >= 5) {
                service_type = desc[2];
                const uint8_t provider_len = desc[3];
                if (4 + provider_len + 1 <= len) {
                    const uint8_t name_len = desc[4 + provider_len];

                    const uint8_t* prov_raw = desc + 4;
                    const uint8_t* name_raw = desc + 5 + provider_len;

                    if (provider_len > 0) {
                        decoder.BeginString();
                        const auto u16 = decoder.DecodeToUTF16(prov_raw, provider_len);
                        provider_name.assign(u16.begin(), u16.end());
                    }
                    if (name_len > 0) {
                        decoder.BeginString();
                        const auto u16 = decoder.DecodeToUTF16(name_raw, name_len);
                        service_name.assign(u16.begin(), u16.end());
                    }

                    if (dbg) {
                        std::size_t nulPos = 0;
                        if (ContainsNul(provider_name, &nulPos)) {
                            DebugLog(L"[SDT][WARN] prov has NUL sid=%u pos=%zu len=%zu dump=%s",
                                (unsigned)service_id, nulPos, provider_name.size(),
                                DumpWStringHex(provider_name).c_str());
                        }
                        if (ContainsNul(service_name, &nulPos)) {
                            DebugLog(L"[SDT][WARN] name has NUL sid=%u pos=%zu len=%zu dump=%s",
                                (unsigned)service_id, nulPos, service_name.size(),
                                DumpWStringHex(service_name).c_str());
                        }

                        DebugLog(L"[SDT] sid=%u type=%u sec=%u/%u tsid=%u onid=%u provLen=%u nameLen=%u prov=\"%s\" name=\"%s\"",
                            (unsigned)service_id, (unsigned)service_type,
                            (unsigned)section_number, (unsigned)last_section_number,
                            (unsigned)transport_stream_id, (unsigned)original_network_id,
                            (unsigned)provider_len, (unsigned)name_len,
                            SanitizeForLog(provider_name).c_str(),
                            SanitizeForLog(service_name).c_str());
                    }
                }
            }

            desc += 2 + len;
        }

        ServiceInfo info{
            service_id,
            service_name,
            service_type,
            section_number,
            free_CA_mode,
            provider_name,
            transport_stream_id,
            original_network_id,
            eit_schedule_flag,
            eit_present_following_flag,
            running_status
        };

        auto it = std::find_if(serviceList.begin(), serviceList.end(),
            [&](const ServiceInfo& s) {
                return s.service_id == info.service_id
                    && s.transport_stream_id == info.transport_stream_id
                    && s.original_network_id == info.original_network_id;
            });
        if (it == serviceList.end()) {
            serviceList.push_back(std::move(info));
        }
        else {
            *it = std::move(info);
        }

        loop_start = desc_end;
    }

    if (dbg && recovered) {
        DebugLog(L"[SDT] recovered=true sections=%zu last=%d services=%zu",
            receivedSections.size(), lastSectionNumber, serviceList.size());
    }
}

const std::vector<ServiceInfo>& SDTFilter::GetServiceList() const
{
    return serviceList;
}

std::vector<ServiceInfo> SDTFilter::GetSortedServiceList() const
{
    std::vector<ServiceInfo> sorted = serviceList; // 元の一覧を保持するため意図的にコピー
    std::sort(sorted.begin(), sorted.end(), [](const ServiceInfo& a, const ServiceInfo& b) {
        return a.section_number < b.section_number;
        });
    return sorted;
}
