#include "bondriver_load.h"

#include <iostream>
#include "SystemInfoManager.hpp"


namespace {
    using CreateBonDriverFunc = IBonDriver * (__stdcall*)(); // BonDriver標準は __stdcall 想定
}

bool BonDriverLoader::Load(const std::wstring& bonDriverPath) noexcept
{
    // すでに読み込み済みなら同一パスは成功扱い
    if (IsLoaded()) {
        if (!loadedPath_.empty() && loadedPath_ == bonDriverPath) return true;
        Unload();
    }

    loadedPath_.clear();

    hBonDriver_ = ::LoadLibraryW(bonDriverPath.c_str());
    if (!hBonDriver_) {
        const DWORD err = ::GetLastError();

        if (err == ERROR_BAD_EXE_FORMAT) {
            SystemInfoLogError(
                L"BonDriverの読み込みに失敗しました。アプリ本体とBonDriverのbit数不一致の可能性があります。path=\"%ls\" error=%lu",
                bonDriverPath.empty() ? L"(empty)" : bonDriverPath.c_str(),
                static_cast<unsigned long>(err));
        }

        std::wcerr << L"[BonDriverLoader] LoadLibraryW failed: " << bonDriverPath
            << L" (err=" << err << L")\n";

        return false;
    }

    auto p = ::GetProcAddress(hBonDriver_, "CreateBonDriver");
    if (!p) {
        const DWORD err = ::GetLastError();
        std::wcerr << L"[BonDriverLoader] GetProcAddress(CreateBonDriver) failed (err=" << err << L")\n";
        ::FreeLibrary(hBonDriver_);
        hBonDriver_ = nullptr;
        return false;
    }

    auto CreateBonDriver = reinterpret_cast<CreateBonDriverFunc>(p);
    IBonDriver* bon = CreateBonDriver();
    if (!bon) {
        std::wcerr << L"[BonDriverLoader] CreateBonDriver returned null.\n";
        ::FreeLibrary(hBonDriver_);
        hBonDriver_ = nullptr;
        return false;
    }

    // IBonDriver2 へ
    bon2_ = dynamic_cast<IBonDriver2*>(bon);
    if (!bon2_) {
        std::wcerr << L"[BonDriverLoader] IBonDriver2 is not supported.\n";
        bon->Release();
        ::FreeLibrary(hBonDriver_);
        hBonDriver_ = nullptr;
        return false;
    }

    // チューナーを開く
    if (!bon2_->OpenTuner()) {
        std::wcerr << L"[BonDriverLoader] OpenTuner failed.\n";
        bon2_->Release();
        bon2_ = nullptr;
        ::FreeLibrary(hBonDriver_);
        hBonDriver_ = nullptr;
        return false;
    }

    loadedPath_ = bonDriverPath;
    std::wcout << L"[BonDriverLoader] Loaded: " << loadedPath_ << L"\n";
    return true;
}

void BonDriverLoader::Unload() noexcept
{
    if (bon2_) {
        // ドライバ実装側の例外があっても、後続の解放を継続する。
        try {
            bon2_->CloseTuner();
        }
        catch (...) {
        }

        try {
            bon2_->Release();
        }
        catch (...) {
        }

        bon2_ = nullptr;
    }
    if (hBonDriver_) {
        (void)::FreeLibrary(hBonDriver_);
        hBonDriver_ = nullptr;
    }
    loadedPath_.clear();
}
