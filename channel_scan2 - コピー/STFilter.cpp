#include "STFilter.hpp"

#include <algorithm>

namespace {
    static bool ReadTsHeaderBasic(const uint8_t* data, std::size_t size,
        uint16_t& pid, bool& pusi, uint8_t& afc, uint8_t& cc,
        std::size_t& payloadPos) noexcept
    {
        if (!data || size < 4) return false;
        if (data[0] != 0x47) return false;

        pusi = (data[1] & 0x40) != 0;
        pid = (uint16_t)(((data[1] & 0x1F) << 8) | data[2]);

        const uint8_t scrambling = (data[3] & 0xC0) >> 6;
        if (scrambling != 0) return false;

        afc = (data[3] >> 4) & 0x03;
        cc = (data[3] & 0x0F);

        payloadPos = 4;
        if (afc & 0x02) {
            if (payloadPos >= size) return false;
            const uint8_t adaptLen = data[payloadPos];
            payloadPos += 1u + adaptLen;
            if (payloadPos > size) return false;
        }
        if ((afc & 0x01) == 0) return false; // payload 無し
        if (payloadPos >= size) return false;
        return true;
    }
}

void STFilter::Reset() noexcept
{
    states.clear();
    recovered.store(false, std::memory_order_release);
    packetCount.store(0, std::memory_order_relaxed);
    pusiCount.store(0, std::memory_order_relaxed);
    sectionCount.store(0, std::memory_order_relaxed);
    invalidCount.store(0, std::memory_order_relaxed);
    ccErrorCount.store(0, std::memory_order_relaxed);
    lastSnapshot = Snapshot{};
}

void STFilter::InputPacket(const uint8_t* data, std::size_t size)
{
    packetCount.fetch_add(1, std::memory_order_relaxed);

    uint16_t pid = 0;
    bool pusi = false;
    uint8_t afc = 0, cc = 0;
    std::size_t p = 0;
    if (!ReadTsHeaderBasic(data, size, pid, pusi, afc, cc, p)) return;

    auto it = states.find(pid);
    const bool hasState = (it != states.end());
    const bool assembling = hasState ? it->second.assembling : false;

    // 非PUSIでアセンブル中でなければ即戻り（軽量化）
    if (!pusi && !assembling) return;

    if (pusi) {
        pusiCount.fetch_add(1, std::memory_order_relaxed);

        if (p >= size) return;
        const uint8_t pointer = data[p];
        p += 1u;
        if (p + pointer > size) {
            if (hasState) {
                it->second.assembling = false;
                it->second.buf.clear();
                it->second.expectedTotal = 0;
                it->second.lastCC = 0xFF;
            }
            return;
        }

        // pointer で前セクション継続を埋める
        if (assembling && pointer > 0 && it->second.expectedTotal > 0) {
            const std::size_t can = (std::min<std::size_t>)(pointer, size - p);
            it->second.buf.insert(it->second.buf.end(), data + p, data + p + can);
            if (it->second.buf.size() >= it->second.expectedTotal) {
                OnSection(it->second.buf.data(), it->second.expectedTotal, pid);
                it->second.assembling = false;
                it->second.buf.clear();
                it->second.expectedTotal = 0;
            }
        }

        p += pointer;

        // 新セクション開始がなければ終わり
        if (p + 3 > size) return;

        // ST 以外は開始しない
        const uint8_t tid = data[p];
        if (tid != 0x72) return;

        // state を用意
        if (!hasState) {
            if (states.size() >= kMaxStatePids) states.clear();
            it = states.emplace(pid, AsmState{}).first;
        }

        AsmState& st = it->second;
        st.assembling = true;
        st.buf.clear();
        st.expectedTotal = 0;
        st.lastCC = cc;

        const uint16_t section_length = (uint16_t)(((data[p + 1] & 0x0F) << 8) | data[p + 2]);
        st.expectedTotal = 3u + section_length;

        st.buf.insert(st.buf.end(), data + p, data + size);

        if (st.buf.size() >= st.expectedTotal) {
            OnSection(st.buf.data(), st.expectedTotal, pid);
            st.assembling = false;
            st.buf.clear();
            st.expectedTotal = 0;
        }
    }
    else {
        // continuation
        if (!hasState || !it->second.assembling || it->second.expectedTotal == 0) return;

        const uint8_t expect = uint8_t((it->second.lastCC + 1) & 0x0F);
        if (cc != expect) {
            ccErrorCount.fetch_add(1, std::memory_order_relaxed);
            it->second.assembling = false;
            it->second.buf.clear();
            it->second.expectedTotal = 0;
            it->second.lastCC = 0xFF;
            return;
        }
        it->second.lastCC = cc;

        it->second.buf.insert(it->second.buf.end(), data + p, data + size);

        if (it->second.buf.size() >= it->second.expectedTotal) {
            OnSection(it->second.buf.data(), it->second.expectedTotal, pid);
            it->second.assembling = false;
            it->second.buf.clear();
            it->second.expectedTotal = 0;
        }
    }
}

void STFilter::OnSection(const uint8_t* sec, std::size_t len, uint16_t pid)
{
    if (!sec || len < 3) {
        invalidCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    sectionCount.fetch_add(1, std::memory_order_relaxed);

    const uint8_t table_id = sec[0];
    if (table_id != 0x72) {
        invalidCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const bool ssi = (sec[1] & 0x80) != 0;
    const uint16_t section_length = (uint16_t)(((sec[1] & 0x0F) << 8) | sec[2]);
    const std::size_t total = 3u + section_length;

    if (total != len) {
        invalidCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Snapshot snap{};
    snap.valid = true;
    snap.table_id = table_id;
    snap.pid = pid;
    snap.section_syntax_indicator = ssi;
    snap.section_length = section_length;

    lastSnapshot = snap;
    recovered.store(true, std::memory_order_release);
}
