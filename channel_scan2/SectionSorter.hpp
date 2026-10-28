#pragma once
#ifndef SECTION_SORTER_HPP
#define SECTION_SORTER_HPP

#include <vector>
#include <cstdint>
#include <set>
#include <algorithm>
#include "ServiceInfo.hpp"

struct SectionEntry
{
    uint8_t section_number = 0;
    std::vector<uint8_t> data;

    SectionEntry(uint8_t num, const std::vector<uint8_t>& d) : section_number(num), data(d) {}
};

class SectionSorter
{
public:
    // セクション追加（内部で section_number を抽出して保持）
    void AddSection(const uint8_t* data, size_t size);

    // ソート済みセクション取得（section_number順）
    std::vector<std::vector<uint8_t>> GetSortedSections() const;

    // すべてのセクションが揃っているか
    bool IsComplete() const;

    // 最後に取得したlast_section_numberの値（有効な場合）
    uint8_t GetLastSectionNumber() const { return lastSectionNumber; }

    // 登録されたセクション数
    size_t GetSectionCount() const { return entries.size(); }

    static void SortBySectionNumber(std::vector<ServiceInfo>& list);

private:
    std::vector<SectionEntry> entries;
    std::set<uint8_t> receivedSections;
    uint8_t lastSectionNumber = 0xFF;
    bool lastSectionNumberValid = false;

    uint8_t ExtractSectionNumber(const uint8_t* data, size_t size) const;
    uint8_t ExtractLastSectionNumber(const uint8_t* data, size_t size) const;
};

#endif // SECTION_SORTER_HPP
