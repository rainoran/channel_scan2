#pragma once
#ifndef BONDRIVER_LOAD_H
#define BONDRIVER_LOAD_H

#include <windows.h>
#include <string>

#include "IBonDriver2.h"

class BonDriverLoader {
private:
    HMODULE     hBonDriver_ = nullptr;
    IBonDriver2* bon2_ = nullptr;
    std::wstring loadedPath_;

public:
    BonDriverLoader() = default;
    BonDriverLoader(const BonDriverLoader&) = delete;
    BonDriverLoader& operator=(const BonDriverLoader&) = delete;

    // --- MainUI が期待しているAPI ---
    bool Load(const std::wstring& bonDriverPath) noexcept;
    void Unload() noexcept;

    bool IsLoaded() const noexcept { return (hBonDriver_ != nullptr) && (bon2_ != nullptr); }
    std::wstring GetLoadedPath() const { return loadedPath_; }
    IBonDriver2* GetBonDriver2() const noexcept { return bon2_; }

    // --- 既存互換（古い呼び名）---
    bool LoadBonDriver(const std::wstring& bonDriverPath) { return Load(bonDriverPath); }
    IBonDriver2* GetBonDriver() const { return bon2_; }
    void Cleanup() { Unload(); }

    ~BonDriverLoader() { Unload(); }
};

#endif // BONDRIVER_LOAD_H
