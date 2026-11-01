#include "EITEventTextAssembler.hpp"

#include <algorithm>
#include <utility>

namespace {
    constexpr uint8_t  kMaxDescNum = 0x0F;
    constexpr std::size_t kMaxFragmentLen = 2048;

    inline bool IsValidDescNum(uint8_t n) noexcept { return n <= kMaxDescNum; }

    // 同番号が複数ある場合は「長い方」を残す。順序は最終的にAssembleで揃えるのでここは軽く。
    inline void UpsertFragment(
        std::vector<EitTextFragment>& frags,
        uint8_t descriptor_number,
        const uint8_t* bytes,
        std::size_t len,
        std::size_t maxFrags)
    {
        auto it = std::find_if(frags.begin(), frags.end(),
            [descriptor_number](const EitTextFragment& f) { return f.descriptor_number == descriptor_number; });

        if (it == frags.end()) {
            if (frags.size() >= maxFrags) return;
            EitTextFragment f;
            f.descriptor_number = descriptor_number;
            f.bytes.assign(bytes, bytes + len);
            frags.emplace_back(std::move(f));
        }
        else {
            if (len >= it->bytes.size()) {
                it->bytes.assign(bytes, bytes + len);
            }
        }
    }
}

// ------------------------------------------------------------
// public static helpers (FIX: access error E0265)
// ------------------------------------------------------------
bool EitEventTextAssembler::IsValidStartTimeRaw(uint64_t raw40) noexcept
{
    // EIT start_time は MJD(2) + BCD(3) の 5byte ＝ 40bit を想定
    // 0 や all-ones は無効として扱う（混線防止）
    return raw40 != 0 && raw40 != 0xFFFFFFFFFFull;
}

uint8_t EitEventTextAssembler::TableIdForKey(uint8_t table_id) noexcept
{
    // 設計変更対策：
    // 0x58-0x5F を 0x50/0x51 に寄せる（同一イベントが別tableで出るケースの混線/分断回避）
    if (table_id >= 0x58 && table_id <= 0x5F) {
        return (table_id & 0x01) ? 0x51 : 0x50;
    }
    // schedule other 側も同様に寄せたい場合（必要なら有効化）
    if (table_id >= 0x68 && table_id <= 0x6F) {
        return (table_id & 0x01) ? 0x61 : 0x60;
    }
    return table_id;
}

EitEventTextKey EitEventTextAssembler::NormalizeKey(const EitEventTextKey& in) noexcept
{
    EitEventTextKey k = in;
    k.table_id = TableIdForKey(k.table_id);
    return k;
}

// ------------------------------------------------------------

EitEventTextAssembler::EitEventTextAssembler() = default;
EitEventTextAssembler::~EitEventTextAssembler() = default;

void EitEventTextAssembler::Reset()
{
    m_events.clear();
}

void EitEventTextAssembler::AddFragment(
    const EitEventTextKey& key,
    uint8_t descriptor_number,
    uint8_t last_descriptor_number,
    bool    lastDescValid,
    const uint8_t* textBytes,
    std::size_t textLength
)
{
    if (!textBytes || textLength == 0) return;

    // start_time が無効だと event_id 再利用等で混線しやすいので捨てる（安全優先）
    if (!IsValidStartTimeRaw(key.start_time_raw)) return;

    if (!IsValidDescNum(descriptor_number)) return;
    if (lastDescValid && !IsValidDescNum(last_descriptor_number)) return;
    if (lastDescValid && descriptor_number > last_descriptor_number) return;

    const EitEventTextKey nkey = NormalizeKey(key);

    // MAX_EVENTS 超過時は古いものを1件落として暴走回避
    auto itEvent = m_events.find(nkey);
    if (itEvent == m_events.end()) {
        if (m_events.size() >= MAX_EVENTS) {
            m_events.erase(m_events.begin());
        }
        itEvent = m_events.emplace(nkey, EitEventFragments{}).first;
    }

    if (textLength > kMaxFragmentLen) textLength = kMaxFragmentLen;

    auto& ev = itEvent->second;

    // ------------------------------
    // last_descriptor_number の扱い
    //  - descriptor_number==0 を「系列の頭」とみなし、
    //    last が変わったら系列を切り替えて断片をクリア（混線防止）
    //  - 0以外でlastが揺れた場合は “更新しない”（別系列混入の可能性が高い）
    // ------------------------------
    if (lastDescValid) {
        if (!ev.has_last) {
            ev.has_last = true;
            ev.last_descriptor_number = last_descriptor_number;
        }
        else if (ev.last_descriptor_number != last_descriptor_number) {
            if (descriptor_number == 0) {
                // 新系列の開始：旧断片を破棄して last を更新
                ev.fragments.clear();
                ev.last_descriptor_number = last_descriptor_number;
            }
            else {
                // 途中でlastが変わるのは混線っぽい：last更新はしない
            }
        }
    }

    // フラグメント格納（同番号は長い方）
    UpsertFragment(ev.fragments, descriptor_number, textBytes, textLength, MAX_FRAGMENTS_PER_EVENT);
}

bool EitEventTextAssembler::IsComplete(const EitEventTextKey& key) const
{
    if (!IsValidStartTimeRaw(key.start_time_raw)) return false;

    const EitEventTextKey nkey = NormalizeKey(key);

    auto it = m_events.find(nkey);
    if (it == m_events.end()) return false;

    const auto& ev = it->second;
    if (!ev.has_last) return false;

    const uint8_t last = ev.last_descriptor_number;

    // 0..last が全部あるか
    std::vector<bool> have(static_cast<std::size_t>(last) + 1, false);
    for (const auto& f : ev.fragments) {
        if (f.descriptor_number <= last) have[f.descriptor_number] = true;
    }
    if (have.empty()) return false;
    for (bool b : have) if (!b) return false;

    return true;
}

bool EitEventTextAssembler::Assemble(const EitEventTextKey& key, std::vector<uint8_t>& out) const
{
    if (!IsValidStartTimeRaw(key.start_time_raw)) return false;

    const EitEventTextKey nkey = NormalizeKey(key);

    auto it = m_events.find(nkey);
    if (it == m_events.end()) return false;

    const auto& ev = it->second;
    if (!ev.has_last) return false;

    const uint8_t last = ev.last_descriptor_number;

    // 0..last の表を作る（重複は“長い方”）
    std::vector<const EitTextFragment*> table(static_cast<std::size_t>(last) + 1, nullptr);
    for (const auto& f : ev.fragments) {
        if (f.descriptor_number > last) continue;
        auto& slot = table[f.descriptor_number];
        if (!slot || f.bytes.size() > slot->bytes.size()) {
            slot = &f;
        }
    }
    for (auto* p : table) if (!p) return false;

    out.clear();
    std::size_t total = 0;
    for (const auto* p : table) total += p->bytes.size();
    out.reserve(total);

    for (const auto* p : table) {
        out.insert(out.end(), p->bytes.begin(), p->bytes.end());
    }
    return true;
}

bool EitEventTextAssembler::AssembleAndErase(const EitEventTextKey& key, std::vector<uint8_t>& out)
{
    const bool ok = Assemble(key, out);
    if (ok) {
        const EitEventTextKey nkey = NormalizeKey(key);
        m_events.erase(nkey);
    }
    return ok;
}
