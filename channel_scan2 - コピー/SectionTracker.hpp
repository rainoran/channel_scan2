#pragma once
#pragma once

#include <vector>
#include <cstdint>
#include "ServiceInfo.hpp"

class SectionTracker
{
public:
    // 最大セクション番号を指定して初期化
    void Initialize(uint8_t lastSectionNumber);

    // セクション番号を既に受信済みか確認
    bool IsDuplicate(uint8_t sectionNumber) const;

    // セクション番号を受信済みにマーク
    bool MarkReceived(uint8_t sectionNumber);

    // すべてのセクションが受信済みか判定
    bool IsAllReceived() const;

    // 抜けているセクション番号を返す
    std::vector<uint8_t> GetMissingSections() const;
    SectionTracker();
    void Reset();
    void MarkSection(uint8_t sectionNumber);
    bool IsComplete() const;
    void SetLastSectionNumber(uint8_t lastSectionNumber);

private:
    uint8_t maxSectionNumber = 0;
    std::vector<bool> receivedFlags;

    std::vector<bool> sectionReceived;
    uint8_t lastSection = 0;
    std::vector<bool> received_sections;
};
