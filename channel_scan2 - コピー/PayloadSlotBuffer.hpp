#pragma once
#pragma once

#include <array>
#include <vector>
#include <cstdint>
#include <cstring>

// 共通エントリ構造
struct PayloadEntry {
    uint8_t continuityCounter;
    std::array<uint8_t, 184> payload;
};

// スロットバッファ（テンプレート）
template<size_t MaxSlots = 8>
class PayloadSlotBufferImpl {
public:
    void AddPayloadToSlot(size_t slotIndex, uint8_t cc, const uint8_t* data)
    {
        if (slotIndex >= MaxSlots || !data) return;
        PayloadEntry entry;
        entry.continuityCounter = cc;
        std::memcpy(entry.payload.data(), data, 184);
        slots[slotIndex].push_back(entry);
    }

    const std::vector<PayloadEntry>& GetSlot(size_t slotIndex) const
    {
        static const std::vector<PayloadEntry> empty;
        if (slotIndex >= MaxSlots) return empty;
        return slots[slotIndex];
    }

    void ClearSlot(size_t slotIndex)
    {
        if (slotIndex >= MaxSlots) return;
        slots[slotIndex].clear();
    }

    void ClearAll()
    {
        for (auto& s : slots) s.clear();
    }

private:
    std::array<std::vector<PayloadEntry>, MaxSlots> slots;
};

// PayloadSlotBuffer 全体（各フィルター用）
class PayloadSlotBuffer
{
public:
    using SDTPayloadSlotBuffer = PayloadSlotBufferImpl<8>;
    using NITPayloadSlotBuffer = PayloadSlotBufferImpl<8>;

    SDTPayloadSlotBuffer& SDT() { return sdtBuffer; }
    NITPayloadSlotBuffer& NIT() { return nitBuffer; }

    void ClearAll()
    {
        sdtBuffer.ClearAll();
        nitBuffer.ClearAll();
    }

private:
    SDTPayloadSlotBuffer sdtBuffer;
    NITPayloadSlotBuffer nitBuffer;
};
