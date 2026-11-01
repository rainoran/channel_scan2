#include "RSTFilter.hpp"

#include <algorithm>
#include <cstring>

// ---- RST は ETSI EN 300 468 Table 10: section_syntax_indicator は 0（CRC/section_number 等なし） ----
// 1 entry = 9 bytes: tsid(2), onid(2), sid(2), eid(2), reserved(5)+running_status(3)

void RSTFilter::Reset() noexcept
{
    recovered_.store(false, std::memory_order_release);

    pidPacketCount_.store(0);
    pusiCount_.store(0);
    sectionCount_.store(0);
    entryCount_.store(0);
    invalidSectionCount_.store(0);
    continuityErrorCount_.store(0);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        latest_.clear();
        updateSeq_ = 0;
    }

    ResetAssembler();
    lastCcValid_ = false;
    lastCc_ = 0xFF;
}

void RSTFilter::ResetAssembler() noexcept
{
    assembling_ = false;
    secPos_ = 0;
    secNeed_ = 0;
}

uint64_t RSTFilter::PackKey(const EventKey& k) noexcept
{
    return (uint64_t(k.tsid) << 48) |
        (uint64_t(k.onid) << 32) |
        (uint64_t(k.sid) << 16) |
        (uint64_t(k.eid));
}

std::vector<std::pair<RSTFilter::EventKey, RSTFilter::EventInfo>> RSTFilter::GetSnapshot() const
{
    std::vector<std::pair<EventKey, EventInfo>> out;
    std::lock_guard<std::mutex> lk(mtx_);
    out.reserve(latest_.size());
    for (const auto& kv : latest_) {
        const uint64_t v = kv.first;
        EventKey k{};
        k.tsid = uint16_t((v >> 48) & 0xFFFF);
        k.onid = uint16_t((v >> 32) & 0xFFFF);
        k.sid = uint16_t((v >> 16) & 0xFFFF);
        k.eid = uint16_t((v >> 0) & 0xFFFF);
        out.emplace_back(k, kv.second);
    }
    return out;
}

void RSTFilter::ProcessSection(const uint8_t* sec, std::size_t totalLen)
{
    // totalLen = 3 + section_length
    if (totalLen < 3) {
        invalidSectionCount_++;
        return;
    }

    const uint8_t table_id = sec[0];
    if (table_id != kTableIdRST) {
        // RST 以外は無視（PIDはRST専用だが安全のため）
        return;
    }

    const bool ssi = (sec[1] & 0x80) != 0;
    // RST は ssi=0 が仕様。ssi=1 は異常扱い。
    if (ssi) {
        invalidSectionCount_++;
        return;
    }

    const uint16_t section_length = uint16_t(((sec[1] & 0x0F) << 8) | sec[2]);
    if (section_length > 1021) {
        invalidSectionCount_++;
        return;
    }
    if (std::size_t(section_length) + 3 != totalLen) {
        invalidSectionCount_++;
        return;
    }

    // entry は 9 バイト固定
    if (section_length < 9 || (section_length % 9) != 0) {
        // 一応、切り捨てで読める分だけ読む運用もあるが、RSTは固定なので異常としてカウント
        invalidSectionCount_++;
        // 読める分だけ読んでもいいが、今回は安全優先で return
        return;
    }

    const uint8_t* p = sec + 3;
    const std::size_t n = section_length / 9;

    std::lock_guard<std::mutex> lk(mtx_);
    for (std::size_t i = 0; i < n; ++i) {
        EventKey key{};
        key.tsid = uint16_t((p[0] << 8) | p[1]);
        key.onid = uint16_t((p[2] << 8) | p[3]);
        key.sid = uint16_t((p[4] << 8) | p[5]);
        key.eid = uint16_t((p[6] << 8) | p[7]);

        const uint8_t rs_byte = p[8];
        const uint8_t running_status = uint8_t(rs_byte & 0x07); // 下位3bit

        const uint64_t packed = PackKey(key);

        EventInfo info{};
        info.running_status = running_status;
        info.update_seq = ++updateSeq_;

        latest_[packed] = info;

        entryCount_++;
        p += 9;
    }

    sectionCount_++;
    recovered_.store(true, std::memory_order_release);
}

void RSTFilter::InputPacket(const uint8_t* ts, std::size_t size)
{
    pidPacketCount_++;

    if (!ts || size < 4) return;
    if (ts[0] != 0x47) return;

    const uint16_t pid = uint16_t(((ts[1] & 0x1F) << 8) | ts[2]);
    if (pid != kPidRST) return;

    const bool pusi = (ts[1] & 0x40) != 0;
    const uint8_t afc = (ts[3] >> 4) & 0x03;
    const uint8_t cc = (ts[3] & 0x0F);

    // payload 無し
    if ((afc & 0x01) == 0) {
        // 連結中に payload が来ないのは基本あり得ないので、組み立て中なら崩す
        if (assembling_) {
            continuityErrorCount_++;
            ResetAssembler();
        }
        lastCcValid_ = true;
        lastCc_ = cc;
        return;
    }

    std::size_t p = 4;
    if (afc & 0x02) {
        if (p >= size) return;
        const uint8_t adapt_len = ts[p];
        p += 1u + adapt_len;
        if (p > size) return;
    }
    if (p >= size) return;

    // continuity check（組み立て中のみ厳密）
    if (assembling_ && lastCcValid_) {
        const uint8_t expect = uint8_t((lastCc_ + 1) & 0x0F);
        if (cc != expect) {
            continuityErrorCount_++;
            ResetAssembler();
        }
    }
    lastCcValid_ = true;
    lastCc_ = cc;

    if (pusi) {
        pusiCount_++;

        // PUSI で始まる場合、pointer_field を解釈する
        if (p >= size) return;
        const uint8_t pointer = ts[p];
        p += 1u;

        // pointer の分だけスキップ（この間に前セクションの残骸が入るケースがあるが、RSTは短いので安全側で捨てる）
        if (p + pointer > size) {
            // 不正 pointer
            invalidSectionCount_++;
            ResetAssembler();
            return;
        }
        p += pointer;

        // PUSI が来たら、未完成の組み立ては破棄（安全側）
        if (assembling_) {
            ResetAssembler();
        }
    }

    // ここから payload 内を走査（複数セクションが連続する場合もある）
    while (p < size) {
        const uint8_t* payload = ts + p;
        const std::size_t avail = size - p;

        if (!assembling_) {
            // セクションヘッダが最低 3 バイト必要
            if (avail < 3) {
                // 次パケットに跨る可能性 → 組み立て開始
                assembling_ = true;
                secPos_ = 0;
                secNeed_ = 3; // 最低でも 3 まで集める
                const std::size_t take = (std::min)(avail, kMaxSectionBytes);
                std::memcpy(secBuf_.data(), payload, take);
                secPos_ = take;
                return;
            }

            const uint8_t table_id = payload[0];
            const bool ssi = (payload[1] & 0x80) != 0;
            const uint16_t section_length = uint16_t(((payload[1] & 0x0F) << 8) | payload[2]);
            const std::size_t totalLen = 3u + std::size_t(section_length);

            if (section_length > 1021 || totalLen > kMaxSectionBytes) {
                invalidSectionCount_++;
                return;
            }

            // RST は table_id=0x71, ssi=0 が基本
            // ここでは “RST以外を見ても安全に読み飛ばす” 方針で組む
            if (avail >= totalLen) {
                // 1パケット内で完結
                ProcessSection(payload, totalLen);
                p += totalLen;
                continue;
            }

            // 跨る → 組み立て
            assembling_ = true;
            secNeed_ = totalLen;
            secPos_ = 0;

            const std::size_t take = (std::min)(avail, secNeed_);
            std::memcpy(secBuf_.data(), payload, take);
            secPos_ = take;
            return;
        }
        else {
            // assembling 中
            if (secNeed_ == 0) {
                // 保険：状態破損
                invalidSectionCount_++;
                ResetAssembler();
                return;
            }

            const std::size_t remain = secNeed_ - secPos_;
            const std::size_t take = (std::min)(remain, avail);

            if (secPos_ + take > kMaxSectionBytes) {
                invalidSectionCount_++;
                ResetAssembler();
                return;
            }

            std::memcpy(secBuf_.data() + secPos_, payload, take);
            secPos_ += take;
            p += take;

            if (secPos_ >= 3 && secNeed_ == 3) {
                // “最初の3バイトだけ欲しい” で開始した場合、ここで totalLen を確定
                const uint16_t section_length = uint16_t(((secBuf_[1] & 0x0F) << 8) | secBuf_[2]);
                const std::size_t totalLen = 3u + std::size_t(section_length);
                if (section_length > 1021 || totalLen > kMaxSectionBytes) {
                    invalidSectionCount_++;
                    ResetAssembler();
                    return;
                }
                secNeed_ = totalLen;
            }

            if (secNeed_ != 0 && secPos_ == secNeed_) {
                // 完成
                ProcessSection(secBuf_.data(), secNeed_);
                ResetAssembler();
                // ループ継続（同一 TS パケット内に続きがある可能性）
                continue;
            }

            // まだ未完成 → 次パケットへ
            if (p >= size) return;
        }
    }
}
