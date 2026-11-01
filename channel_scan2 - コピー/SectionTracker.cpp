#include "SectionTracker.hpp"

SectionTracker::SectionTracker()
    : sectionReceived(256, false), lastSection(0) {
}

void SectionTracker::Reset()
{
    std::fill(sectionReceived.begin(), sectionReceived.end(), false);
    lastSection = 0;
}

void SectionTracker::SetLastSectionNumber(uint8_t lastSectionNumber)
{
    lastSection = lastSectionNumber;
}

void SectionTracker::MarkSection(uint8_t sectionNumber)
{
    if (sectionNumber < sectionReceived.size())
    {
        sectionReceived[sectionNumber] = true;
    }
}

bool SectionTracker::IsComplete() const
{
    for (uint8_t i = 0; i <= lastSection; ++i)
    {
        if (!sectionReceived[i])
            return false;
    }
    return true;
}

void SectionTracker::Initialize(uint8_t lastSectionNumber)
{
    maxSectionNumber = lastSectionNumber;
    receivedFlags.assign(static_cast<size_t>(lastSectionNumber + 1), false);
}

bool SectionTracker::IsDuplicate(uint8_t sectionNumber) const
{
    if (sectionNumber > maxSectionNumber) return false;
    return receivedFlags[sectionNumber];
}

bool SectionTracker::MarkReceived(uint8_t sectionNumber)
{
    if (received_sections.size() <= sectionNumber)
        received_sections.resize(sectionNumber + 1, false);

    if (!received_sections[sectionNumber])
    {
        received_sections[sectionNumber] = true;
        return true;  // 初めての受信
    }

    return false;     // すでに受信済み
}


bool SectionTracker::IsAllReceived() const
{
    for (size_t i = 0; i <= maxSectionNumber; ++i)
    {
        if (!receivedFlags[i])
            return false;
    }
    return true;
}

std::vector<uint8_t> SectionTracker::GetMissingSections() const
{
    std::vector<uint8_t> missing;
    for (size_t i = 0; i <= maxSectionNumber; ++i)
    {
        if (!receivedFlags[i])
            missing.push_back(static_cast<uint8_t>(i));
    }
    return missing;
}
