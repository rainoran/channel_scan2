#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

#include "EITTextDumpLoader.hpp"
#include "EpgModel.hpp" 

// ------------------------------------------------------------
// EPG ID ソートユニット
//   ・EITTextEvent を (SID/TSID/ONID) → 開始時刻 の順で並べ替える
//   ・開始時刻は startTimeRawHex を 64bit のキーにしてソート
// ------------------------------------------------------------



// startTimeRawHex("0xEE52052500" など) を 64bit の数値に変換してソート用キーにする
inline uint64_t ParseStartTimeKey(const std::wstring& rawHex) noexcept
{
    if (rawHex.empty())
        return 0;

    std::wstring s = rawHex;

    // 先頭が "0x" / "0X" なら削る
    if (s.size() > 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X'))
        s.erase(0, 2);

    uint64_t value = 0;
    for (wchar_t ch : s) {
        int d = 0;
        if (ch >= L'0' && ch <= L'9')
            d = ch - L'0';
        else if (ch >= L'a' && ch <= L'f')
            d = 10 + (ch - L'a');
        else if (ch >= L'A' && ch <= L'F')
            d = 10 + (ch - L'A');
        else
            break;

        value = (value << 4) | static_cast<uint64_t>(d);
    }
    return value;
}

// ソート用ビュー
struct EpgSortViewItem
{
    const EITTextEvent* event = nullptr;  // 元の EITTextEvent へのポインタ
    uint64_t            serviceKey = 0;     // SID/TSID/ONID から作ったキー
    uint64_t            startKey = 0;     // startTimeRawHex から作った時間キー
};

// events → ソート用ビューに変換
inline void BuildEpgSortView(
    const std::vector<EITTextEvent>& events,
    std::vector<EpgSortViewItem>& view)
{
    view.clear();
    view.reserve(events.size());

    for (const auto& ev : events) {
        EpgSortViewItem v;
        v.event = &ev;
        v.serviceKey = MakeServiceKey(ev.serviceId, ev.tsid, ev.onid);
        v.startKey = ParseStartTimeKey(ev.startTimeRawHex);
        view.push_back(v);
    }
}

// serviceKey → startKey の順でソート
inline void SortEpgViewByServiceAndStart(std::vector<EpgSortViewItem>& view)
{
    std::sort(
        view.begin(), view.end(),
        [](const EpgSortViewItem& a, const EpgSortViewItem& b)
        {
            if (a.serviceKey != b.serviceKey)
                return a.serviceKey < b.serviceKey;
            return a.startKey < b.startKey;
        });
}
