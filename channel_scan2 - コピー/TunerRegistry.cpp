#include "TunerRegistry.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace
{
    std::wstring MakeTunerSection_(int tunerIndex)
    {
        if (tunerIndex < 0) {
            tunerIndex = 0;
        }

        return L"Tuner" + std::to_wstring(tunerIndex);
    }

    std::wstring MakeIndexedKey_(const wchar_t* baseKey, int tunerIndex)
    {
        if (baseKey == nullptr || baseKey[0] == L'\0') {
            return {};
        }

        if (tunerIndex < 0) {
            tunerIndex = 0;
        }

        return std::wstring(baseKey) + std::to_wstring(tunerIndex);
    }
}

bool TunerRegistry::Initialize(const std::wstring& iniPath, int tunerCount)
{
    {
        std::lock_guard lk(mtx_);

        iniPath_ = iniPath;
        tunerCount_ = (tunerCount <= 0) ? 1 : tunerCount;
        activeIndex_ = 0;

        slots_.assign(static_cast<size_t>(tunerCount_), Slot{});

        for (int i = 0; i < tunerCount_; ++i) {
            Slot& s = slots_[static_cast<size_t>(i)];
            s.index = i;
            s.enabled = true;
            s.name = L"Tuner" + std::to_wstring(i);
            s.bonPath.clear();
        }
    }

    ReloadFromIni();
    return true;
}

std::wstring TunerRegistry::ReadIniString_(const wchar_t* sec, const wchar_t* key, const wchar_t* defv) const
{
    wchar_t buf[1024]{};

    ::GetPrivateProfileStringW(
        sec,
        key,
        defv,
        buf,
        static_cast<DWORD>(_countof(buf)),
        iniPath_.c_str());

    return std::wstring(buf);
}

int TunerRegistry::ReadIniInt_(const wchar_t* sec, const wchar_t* key, int defv) const
{
    return static_cast<int>(
        ::GetPrivateProfileIntW(
            sec,
            key,
            defv,
            iniPath_.c_str()));
}

void TunerRegistry::WriteIniString_(const wchar_t* sec, const wchar_t* key, const std::wstring& v) const
{
    ::WritePrivateProfileStringW(
        sec,
        key,
        v.c_str(),
        iniPath_.c_str());
}

void TunerRegistry::WriteIniInt_(const wchar_t* sec, const wchar_t* key, int v) const
{
    WriteIniString_(sec, key, std::to_wstring(v));
}

std::wstring TunerRegistry::ResolvePath_(const std::wstring& raw) const
{
    if (raw.empty()) {
        return {};
    }

    std::filesystem::path p(raw);

    if (p.is_absolute()) {
        return p.wstring();
    }

    std::filesystem::path base(iniPath_);
    return (base.parent_path() / p).lexically_normal().wstring();
}

void TunerRegistry::ReloadFromIni()
{
    std::lock_guard lk(mtx_);

    activeIndex_ = ReadIniInt_(L"Tuners", L"ActiveIndex", 0);

    if (activeIndex_ < 0) {
        activeIndex_ = 0;
    }

    if (activeIndex_ >= tunerCount_) {
        activeIndex_ = tunerCount_ - 1;
    }

    for (int i = 0; i < tunerCount_; ++i) {
        Slot& s = slots_[static_cast<size_t>(i)];

        const std::wstring section = MakeTunerSection_(i);
        const std::wstring keyPath = MakeIndexedKey_(L"BonDriverPath", i);
        const std::wstring keyName = MakeIndexedKey_(L"Name", i);
        const std::wstring keyEnable = MakeIndexedKey_(L"Enable", i);

        s.index = i;
        s.enabled =
            ReadIniInt_(
                section.c_str(),
                keyEnable.c_str(),
                1) != 0;

        s.name =
            ReadIniString_(
                section.c_str(),
                keyName.c_str(),
                L"");

        if (s.name.empty()) {
            s.name = L"Tuner" + std::to_wstring(i);
        }

        s.bonPath =
            ResolvePath_(
                ReadIniString_(
                    section.c_str(),
                    keyPath.c_str(),
                    L""));
    }
}

void TunerRegistry::SetActiveIndex(int idx, bool saveToIni)
{
    std::lock_guard lk(mtx_);

    if (idx < 0) {
        idx = 0;
    }

    if (idx >= tunerCount_) {
        idx = tunerCount_ - 1;
    }

    activeIndex_ = idx;

    if (saveToIni) {
        WriteIniInt_(L"Tuners", L"ActiveIndex", activeIndex_);
    }
}

TunerRegistry::Slot TunerRegistry::GetSlot(int idx) const
{
    std::lock_guard lk(mtx_);

    if (idx < 0 || idx >= tunerCount_) {
        return Slot{};
    }

    return slots_[static_cast<size_t>(idx)];
}

void TunerRegistry::SetSlot(const Slot& in, bool saveToIni)
{
    std::lock_guard lk(mtx_);

    if (in.index < 0 || in.index >= tunerCount_) {
        return;
    }

    Slot& s = slots_[static_cast<size_t>(in.index)];

    s.index = in.index;
    s.enabled = in.enabled;
    s.name = in.name;
    s.bonPath = in.bonPath;

    if (!saveToIni) {
        return;
    }

    const std::wstring section = MakeTunerSection_(in.index);
    const std::wstring keyPath = MakeIndexedKey_(L"BonDriverPath", in.index);
    const std::wstring keyName = MakeIndexedKey_(L"Name", in.index);
    const std::wstring keyEnable = MakeIndexedKey_(L"Enable", in.index);

    WriteIniString_(section.c_str(), keyPath.c_str(), s.bonPath);
    WriteIniString_(section.c_str(), keyName.c_str(), s.name);
    WriteIniInt_(section.c_str(), keyEnable.c_str(), s.enabled ? 1 : 0);
}

std::wstring TunerRegistry::GetBonPathForTunerOrDefault(int idx) const
{
    std::lock_guard lk(mtx_);

    if (0 <= idx && idx < tunerCount_) {
        const std::wstring p = slots_[static_cast<size_t>(idx)].bonPath;

        if (!p.empty()) {
            return p;
        }
    }

    // 最後の保険。旧単体運用用。
    return ResolvePath_(ReadIniString_(L"BonDriver", L"Path", L""));
}