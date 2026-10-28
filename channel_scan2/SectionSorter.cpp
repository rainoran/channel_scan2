#include "SectionSorter.hpp"
#include <cstring>

void SectionSorter::AddSection(const uint8_t* data, size_t size)
{
    if (!data || size < 8)
        return;

    uint8_t section_number = ExtractSectionNumber(data, size);
    uint8_t last_number = ExtractLastSectionNumber(data, size);

    // すでに取得済みのセクションは無視
    if (receivedSections.count(section_number))
        return;

    entries.emplace_back(section_number, std::vector<uint8_t>(data, data + size));
    receivedSections.insert(section_number);

    lastSectionNumber = last_number;
    lastSectionNumberValid = true;
}

std::vector<std::vector<uint8_t>> SectionSorter::GetSortedSections() const
{
    std::vector<SectionEntry> sorted = entries;

    std::sort(sorted.begin(), sorted.end(),
        [](const SectionEntry& a, const SectionEntry& b)
        {
            return a.section_number < b.section_number;
        });

    std::vector<std::vector<uint8_t>> result;
    for (const auto& entry : sorted)
    {
        result.push_back(entry.data);
    }
    return result;
}

bool SectionSorter::IsComplete() const
{
    if (!lastSectionNumberValid)
        return false;

    for (uint8_t i = 0; i <= lastSectionNumber; ++i)
    {
        if (receivedSections.count(i) == 0)
            return false;
    }
    return true;
}

uint8_t SectionSorter::ExtractSectionNumber(const uint8_t* data, size_t size) const
{
    if (size < 8)
        return 0xFF;
    return data[6];
}

uint8_t SectionSorter::ExtractLastSectionNumber(const uint8_t* data, size_t size) const
{
    if (size < 8)
        return 0xFF;
    return data[7];
}

void SectionSorter::SortBySectionNumber(std::vector<ServiceInfo>& list)
{
    std::sort(list.begin(), list.end(), [](const ServiceInfo& a, const ServiceInfo& b) {
        return a.section_number < b.section_number;
        });
}