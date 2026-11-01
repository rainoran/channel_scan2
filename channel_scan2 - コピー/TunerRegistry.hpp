#pragma once
#pragma once
#ifndef TUNER_REGISTRY_HPP
#define TUNER_REGISTRY_HPP

#include <Windows.h>
#include <string>
#include <vector>
#include <mutex>

class TunerRegistry final
{
public:
    struct Slot {
        int index = 0;
        bool enabled = true;
        std::wstring name;
        std::wstring bonPath;   // 絶対パス推奨
    };

public:
    bool Initialize(const std::wstring& iniPath, int tunerCount);
    void ReloadFromIni();

    int GetTunerCount() const noexcept { return tunerCount_; }

    int GetActiveIndex() const noexcept { return activeIndex_; }
    void SetActiveIndex(int idx, bool saveToIni = true);

    Slot GetSlot(int idx) const;
    void SetSlot(const Slot& s, bool saveToIni = true);

    std::wstring GetBonPathForTunerOrDefault(int idx) const;

private:
    std::wstring ReadIniString_(const wchar_t* sec, const wchar_t* key, const wchar_t* defv) const;
    int ReadIniInt_(const wchar_t* sec, const wchar_t* key, int defv) const;
    void WriteIniString_(const wchar_t* sec, const wchar_t* key, const std::wstring& v) const;
    void WriteIniInt_(const wchar_t* sec, const wchar_t* key, int v) const;

    std::wstring ResolvePath_(const std::wstring& raw) const;

private:
    mutable std::mutex mtx_;
    std::wstring iniPath_;
    int tunerCount_ = 1;
    int activeIndex_ = 0;
    std::vector<Slot> slots_;
};

#endif // TUNER_REGISTRY_HPP