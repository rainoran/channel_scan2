#pragma once
#pragma once
#ifndef GUI_SERVICE_SNAPSHOT_HPP
#define GUI_SERVICE_SNAPSHOT_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace GuiSnapshot {

    struct ServiceRow
    {
        uint16_t service_id = 0;
        uint16_t transport_stream_id = 0;
        uint16_t original_network_id = 0;

        uint8_t  service_type = 0;
        uint8_t  section_number = 0;
        bool     free_ca_mode = false;

        bool     eit_schedule_flag = false;
        bool     eit_present_following_flag = false;
        uint8_t  running_status = 0;

        std::wstring provider_name;
        std::wstring service_name;
    };

    // Win32表示が途切れる/崩れる事故を避ける軽い正規化（必要ならON）
    inline void NormalizeForWin32(std::wstring& s, wchar_t nulReplace = L' ')
    {
        for (auto& ch : s) {
            if (ch == L'\0') ch = nulReplace;
            else if (ch < 0x20 && ch != L'\n' && ch != L'\r' && ch != L'\t') ch = L' ';
        }
    }

    // ServiceInfo -> ServiceRow への変換（ServiceInfo側のメンバ名に合わせて調整）
    template<class ServiceInfoT>
    inline std::vector<ServiceRow> MakeSnapshot(const std::vector<ServiceInfoT>& src, bool normalizeWin32)
    {
        std::vector<ServiceRow> out;
        out.reserve(src.size());

        for (const auto& s : src) {
            ServiceRow r{};
            r.service_id = s.service_id;
            r.transport_stream_id = s.transport_stream_id;
            r.original_network_id = s.original_network_id;

            r.service_type = s.service_type;
            r.section_number = s.section_number;
            r.free_ca_mode = s.free_CA_mode; // ←ServiceInfoの名前が違うなら調整

            r.eit_schedule_flag = s.eit_schedule_flag;
            r.eit_present_following_flag = s.eit_present_following_flag;
            r.running_status = s.running_status;

            r.provider_name = s.provider_name;
            r.service_name = s.service_name;

            if (normalizeWin32) {
                NormalizeForWin32(r.provider_name);
                NormalizeForWin32(r.service_name);
            }

            out.push_back(std::move(r));
        }

        return out;
    }

    // 表示用に並び替え（必要ならキーを変更）
    inline void SortBySectionThenSid(std::vector<ServiceRow>& rows)
    {
        std::sort(rows.begin(), rows.end(), [](const ServiceRow& a, const ServiceRow& b) {
            if (a.section_number != b.section_number) return a.section_number < b.section_number;
            return a.service_id < b.service_id;
            });
    }

} // namespace GuiSnapshot

#endif // GUI_SERVICE_SNAPSHOT_HPP
