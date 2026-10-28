#pragma once
#ifndef BONDRIVER_SCAN_H
#define BONDRIVER_SCAN_H

#include <string>
#include <vector>
#include <system_error>

struct BonDriverScanResult
{
    std::vector<std::wstring> list; // フルパス
    std::error_code ec{};          // ec.value()!=0 ならエラー
};

// 既存互換：失敗時は空（GUIは Ex 推奨）
std::vector<std::wstring> ScanBonDriverFiles(const std::wstring& searchPath);

// GUI向け：例外なし・エラーコード付き・再帰オプションあり
BonDriverScanResult ScanBonDriverFilesEx(const std::wstring& searchPath, bool recursive = false) noexcept;

// CUI用（GUIでは通常不要）
std::wstring SelectBonDriver(const std::vector<std::wstring>& driverList);

// 既存互換：戻り値なし
void SaveSelectedBonDriverToIni(const std::wstring& iniPath, const std::wstring& bonDriverPath);

// GUI向け：保存値（相対 or 絶対）も返せる
std::error_code SaveSelectedBonDriverToIniEx(
    const std::wstring& iniPath,
    const std::wstring& bonDriverPath,
    std::wstring* outWrittenValue = nullptr) noexcept;

#endif // BONDRIVER_SCAN_H
