#include "bondriver_scan.h"

#include <Windows.h>
#include <filesystem>
#include <algorithm>

#include <Shlwapi.h>  // PathRelativePathToW
#pragma comment(lib, "Shlwapi.lib")

namespace fs = std::filesystem;

namespace {

    inline wchar_t LowerW(wchar_t c) noexcept { return (wchar_t)towlower(c); }

    bool IEqualsW(std::wstring_view a, std::wstring_view b) noexcept
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (LowerW(a[i]) != LowerW(b[i])) return false;
        }
        return true;
    }

    bool IStartsWithW(std::wstring_view s, std::wstring_view prefix) noexcept
    {
        if (s.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (LowerW(s[i]) != LowerW(prefix[i])) return false;
        }
        return true;
    }

    bool IsBonDriverDll(const fs::path& p) noexcept
    {
        const std::wstring ext = p.extension().wstring();
        if (!IEqualsW(ext, L".dll")) return false;

        const std::wstring name = p.filename().wstring();
        if (!IStartsWithW(name, L"BonDriver")) return false;

        return true;
    }

    void SortUnique(std::vector<std::wstring>& v)
    {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }

    // iniDir を必ず "...\\" で終わらせる
    void EnsureTrailingBackslash(std::wstring& dir)
    {
        if (dir.empty()) return;
        wchar_t last = dir.back();
        if (last != L'\\' && last != L'/') dir.push_back(L'\\');
    }

    std::error_code GetFullPath(const std::wstring& in, std::wstring& out) noexcept
    {
        out.clear();
        wchar_t buf[MAX_PATH]{};
        DWORD n = GetFullPathNameW(in.c_str(), MAX_PATH, buf, nullptr);
        if (n == 0) return std::error_code((int)GetLastError(), std::system_category());
        out = buf;
        return {};
    }

    std::error_code GetDirectoryFromFullPath(const std::wstring& fullPath, std::wstring& outDir) noexcept
    {
        outDir = fullPath;
        for (int i = (int)outDir.size() - 1; i >= 0; --i) {
            if (outDir[(size_t)i] == L'\\' || outDir[(size_t)i] == L'/') {
                outDir.resize((size_t)i + 1); // 区切りを残す（末尾 \ を確保）
                return {};
            }
        }
        return std::make_error_code(std::errc::invalid_argument);
    }

} // namespace

BonDriverScanResult ScanBonDriverFilesEx(const std::wstring& searchPath, bool recursive) noexcept
{
    BonDriverScanResult r{};

    if (searchPath.empty()) {
        r.ec = std::make_error_code(std::errc::invalid_argument);
        return r;
    }

    std::error_code ec;
    fs::path root(searchPath);

    if (!fs::exists(root, ec) || ec) {
        r.ec = ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory);
        return r;
    }
    if (!fs::is_directory(root, ec) || ec) {
        r.ec = ec ? ec : std::make_error_code(std::errc::not_a_directory);
        return r;
    }

    const auto opts = fs::directory_options::skip_permission_denied;

    if (recursive) {
        fs::recursive_directory_iterator it(root, opts, ec), end;
        if (ec) { r.ec = ec; return r; }

        for (; it != end; it.increment(ec)) {
            if (ec) { r.ec = ec; break; }

            std::error_code ec2;
            if (!it->is_regular_file(ec2) || ec2) continue;

            const fs::path p = it->path();
            if (IsBonDriverDll(p)) r.list.push_back(p.wstring());
        }
    }
    else {
        fs::directory_iterator it(root, opts, ec), end;
        if (ec) { r.ec = ec; return r; }

        for (; it != end; it.increment(ec)) {
            if (ec) { r.ec = ec; break; }

            std::error_code ec2;
            if (!it->is_regular_file(ec2) || ec2) continue;

            const fs::path p = it->path();
            if (IsBonDriverDll(p)) r.list.push_back(p.wstring());
        }
    }

    SortUnique(r.list);
    return r;
}

std::vector<std::wstring> ScanBonDriverFiles(const std::wstring& searchPath)
{
    auto r = ScanBonDriverFilesEx(searchPath, false);
    return std::move(r.list);
}

// CUI用（GUIでは通常不要）
std::wstring SelectBonDriver(const std::vector<std::wstring>& driverList)
{
    // ※ここはあなたの元実装のままでOK（GUIでは使わない想定）
    if (driverList.empty()) return L"";
    return driverList.front();
}

void SaveSelectedBonDriverToIni(const std::wstring& iniPath, const std::wstring& bonDriverPath)
{
    (void)SaveSelectedBonDriverToIniEx(iniPath, bonDriverPath, nullptr);
}

std::error_code SaveSelectedBonDriverToIniEx(
    const std::wstring& iniPath,
    const std::wstring& bonDriverPath,
    std::wstring* outWrittenValue) noexcept
{
    if (outWrittenValue) outWrittenValue->clear();

    // ini のフルパス
    std::wstring iniFull;
    if (auto ec = GetFullPath(iniPath, iniFull); ec) return ec;

    // ini のディレクトリ（末尾 \ 付き）
    std::wstring iniDir;
    if (auto ec = GetDirectoryFromFullPath(iniFull, iniDir); ec) return ec;
    EnsureTrailingBackslash(iniDir);

    // BonDriver のフルパス
    std::wstring bonFull;
    if (auto ec = GetFullPath(bonDriverPath, bonFull); ec) return ec;

    // 相対パス化
    wchar_t relativePath[MAX_PATH]{};
    bool okRel = !!PathRelativePathToW(
        relativePath,
        iniDir.c_str(), FILE_ATTRIBUTE_DIRECTORY,
        bonFull.c_str(), FILE_ATTRIBUTE_NORMAL);

    std::wstring toWrite = okRel ? std::wstring(relativePath) : bonFull;
    if (outWrittenValue) *outWrittenValue = toWrite;

    if (!WritePrivateProfileStringW(L"BonDriver", L"Path", toWrite.c_str(), iniFull.c_str())) {
        return std::error_code((int)GetLastError(), std::system_category());
    }
    return {};
}
