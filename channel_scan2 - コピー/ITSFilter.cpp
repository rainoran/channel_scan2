#include "ITSFilter.hpp"
#include <cstring>
#include "TSSectionValidator.hpp"

ITSFilter::ITSFilter() {
    Reset();
}

void ITSFilter::Reset() {
    recovered = false;
    parsedSectionNumbers.clear();
    buffer.clear();
    bufferIndex = 0;
}

void ITSFilter::InputPacket(const uint8_t* data, size_t size) {
    if (size < 3 || data[0] != 0x47) return;

    uint16_t pid = ((data[1] & 0x1F) << 8) | data[2];
    if (pid != 0x001C) return; // ITS PID

    uint8_t payload_unit_start_indicator = (data[1] & 0x40) >> 6;
    size_t payload_offset = 4;
    if (data[3] & 0x20) {
        payload_offset += data[4] + 1;
    }

    if (payload_offset >= size) return;

    if (payload_unit_start_indicator) {
        if (!buffer.empty()) {
            ParseSection(buffer.data(), buffer.size());
        }
        buffer.clear();
        bufferIndex = 0;
        uint8_t pointerField = data[payload_offset];
        payload_offset += 1 + pointerField;
        if (payload_offset >= size) return;
    }

    buffer.insert(buffer.end(), data + payload_offset, data + size);
}

void ITSFilter::ParseSection(const uint8_t* data, size_t size) {
    if (!TSValidator::IsValidSection(data, size)) return;
    if (size < 8) return;

    uint8_t section_number = data[6];
    if (parsedSectionNumbers.find(section_number) != parsedSectionNumbers.end()) return;

    parsedSectionNumbers.insert(section_number);

    uint8_t last_section_number = data[7];
    if (parsedSectionNumbers.size() > last_section_number) {
        recovered = true;
    }

    // 将来的に ITS テーブルに対応した情報抽出もここで追加可能
}

bool ITSFilter::IsRecovered() const {
    return recovered;
}
