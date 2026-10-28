#include "SIMDselect.h"
#include "configmanager.h"
#include <iostream>
#include <array>
#include <intrin.h>  // Windows用 (GCC, Clang の場合は `<cpuid.h>`)

#ifdef _WIN32
    #include <windows.h>
#endif

#include <locale>
#include <codecvt>

// **C++17 以降 & クロスプラットフォーム対応の wchar_t → char 変換**
std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

#if defined(_WIN32) || defined(_WIN64)
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size_needed, NULL, NULL);
    return str;
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(wstr);
#endif
}

// **CPUベンダー判定**
enum class CPUVendor { UNKNOWN, INTEL, AMD };

CPUVendor GetCPUVendor() {
    std::array<int, 4> cpuInfo{};
    __cpuid(cpuInfo.data(), 0);

    std::string vendor;
    vendor += std::string(reinterpret_cast<char*>(&cpuInfo[1]), 4);
    vendor += std::string(reinterpret_cast<char*>(&cpuInfo[3]), 4);
    vendor += std::string(reinterpret_cast<char*>(&cpuInfo[2]), 4);

    if (vendor == "GenuineIntel") return CPUVendor::INTEL;
    if (vendor == "AuthenticAMD") return CPUVendor::AMD;
    return CPUVendor::UNKNOWN;
}

// **関数ポインタの宣言**
void (*SIMDfunction)(void) = SIMD_SCALAR;  // **デフォルトを SCALAR にする**

void Intel_SSE2() { std::cout << "[INFO] Intel: Using SSE2 optimization.\n"; }
void Intel_SSE4_2() { std::cout << "[INFO] Intel: Using SSE4.2 optimization.\n"; }
void Intel_AVX() { std::cout << "[INFO] Intel: Using AVX optimization.\n"; }
void Intel_AVX2() { std::cout << "[INFO] Intel: Using AVX2 optimization.\n"; }
void Intel_AVX512() { std::cout << "[INFO] Intel: Using AVX512 optimization.\n"; }

void AMD_SSE2() { std::cout << "[INFO] AMD: Using SSE2 optimization.\n"; }
void AMD_SSE4_2() { std::cout << "[INFO] AMD: Using SSE4.2 optimization.\n"; }
void AMD_AVX() { std::cout << "[INFO] AMD: Using AVX optimization.\n"; }
void AMD_AVX2() { std::cout << "[INFO] AMD: Using AVX2 optimization.\n"; }
void AMD_AVX512() { std::cout << "[INFO] AMD: Using AVX512 optimization.\n"; }

void SIMD_SCALAR() { std::cout << "[INFO] Using SCALAR processing.\n"; }  // **SCALAR 処理用関数を定義**

std::string SelectBestSIMD() {
    ConfigManager& config = ConfigManager::GetInstance();
    CPUVendor vendor = GetCPUVendor();

    std::wstring storedFeature = config.GetSelectedCPUFeature();
    std::string selectedFeature = "SCALAR";

    if (!storedFeature.empty()) {
        selectedFeature = WStringToString(storedFeature);
    }

    if (selectedFeature != "SSE2" && selectedFeature != "SSE4.2" &&
        selectedFeature != "AVX" && selectedFeature != "AVX2" && selectedFeature != "AVX512") {
        selectedFeature = "SCALAR";
    }

    if (vendor == CPUVendor::INTEL) {
        if (selectedFeature == "SSE2") SIMDfunction = Intel_SSE2;
        else if (selectedFeature == "SSE4.2") SIMDfunction = Intel_SSE4_2;
        else if (selectedFeature == "AVX") SIMDfunction = Intel_AVX;
        else if (selectedFeature == "AVX2") SIMDfunction = Intel_AVX2;
        else if (selectedFeature == "AVX512") SIMDfunction = Intel_AVX512;
    } else if (vendor == CPUVendor::AMD) {
        if (selectedFeature == "SSE2") SIMDfunction = AMD_SSE2;
        else if (selectedFeature == "SSE4.2") SIMDfunction = AMD_SSE4_2;
        else if (selectedFeature == "AVX") SIMDfunction = AMD_AVX;
        else if (selectedFeature == "AVX2") SIMDfunction = AMD_AVX2;
        else if (selectedFeature == "AVX512") SIMDfunction = AMD_AVX512;
    } else {
        SIMDfunction = SIMD_SCALAR;
    }

    config.SetSelectedCPUFeature(std::wstring(selectedFeature.begin(), selectedFeature.end()));
    config.SaveConfig();

    std::cout << "Selected SIMD Feature: " << selectedFeature << std::endl;
    return selectedFeature;
}
