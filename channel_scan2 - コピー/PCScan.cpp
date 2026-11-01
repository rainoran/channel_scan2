#include "PCScan.h"
#include <iostream>
#include <fstream>
#include <intrin.h>
#include <windows.h>
#include <sysinfoapi.h>


PCScan::PCScan() {
    DetectCPU();
    DetectMemory();
    DetectGPU();
    DetectStorage();
    DetectCPUFeatures();
}

PCScan& PCScan::GetInstance() {
    static PCScan instance;
    return instance;
}


bool PCScan::IsFeatureEnabled(const std::string& feature) const {
    if (feature == "AVX2") return hasAVX2;
    if (feature == "SSE4.2") return hasSSE4_2;
    return false;
}

void PCScan::DetectCPU() {
    int cpuInfo[4] = { 0 };
    __cpuid(cpuInfo, 1);

    hasAVX2 = (cpuInfo[1] & (1 << 5)) != 0;
    hasAVX512 = (cpuInfo[1] & (1 << 16)) != 0;
}

void PCScan::DetectMemory() {
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        totalRAM = memStatus.ullTotalPhys / (1024 * 1024);
        availableRAM = memStatus.ullAvailPhys / (1024 * 1024);
    }
}

void PCScan::DetectGPU() {
    DISPLAY_DEVICE dd;
    dd.cb = sizeof(dd);
    int deviceIndex = 0;

    while (EnumDisplayDevices(nullptr, deviceIndex, &dd, 0)) {
        std::wstring deviceName = dd.DeviceString;
        if (deviceName.find(L"NVIDIA") != std::wstring::npos) {
            hasNvidiaGPU = true;
            gpuVendor = "NVIDIA";
        }
        else if (deviceName.find(L"AMD") != std::wstring::npos) {
            hasAmdGPU = true;
            gpuVendor = "AMD";
        }
        else if (deviceName.find(L"Intel") != std::wstring::npos) {
            hasIntelGPU = true;
            gpuVendor = "Intel";
        }
        deviceIndex++;
    }
}

void PCScan::DetectStorage() {
    ULARGE_INTEGER freeBytesAvailable;
    if (GetDiskFreeSpaceEx(L"C:\\", &freeBytesAvailable, nullptr, nullptr)) {
        freeStorageGB = freeBytesAvailable.QuadPart / (1024 * 1024 * 1024);
    }
}

void PCScan::DetectCPUFeatures() {
    int cpuInfo[4] = { 0 };

    // ベンダー取得
    __cpuid(cpuInfo, 0);
    char vendor[13] = { 0 };
    memcpy(vendor, &cpuInfo[1], 4);
    memcpy(vendor + 4, &cpuInfo[3], 4);
    memcpy(vendor + 8, &cpuInfo[2], 4);
    cpuVendor = std::string(vendor);

    // 拡張機能の取得
    __cpuid(cpuInfo, 1);
    hasSSE2 = (cpuInfo[3] & (1 << 26)) != 0;
    hasSSE3 = (cpuInfo[2] & (1 << 0)) != 0;
    hasSSSE3 = (cpuInfo[2] & (1 << 9)) != 0;
    hasSSE4_1 = (cpuInfo[2] & (1 << 19)) != 0;
    hasSSE4_2 = (cpuInfo[2] & (1 << 20)) != 0;
    hasAVX = (cpuInfo[2] & (1 << 28)) != 0;
    hasFMA = (cpuInfo[2] & (1 << 12)) != 0;
    hasAES_NI = (cpuInfo[2] & (1 << 25)) != 0;
    hasPOPCNT = (cpuInfo[2] & (1 << 23)) != 0;

    __cpuid(cpuInfo, 7);
    hasAVX2 = (cpuInfo[1] & (1 << 5)) != 0;
    hasAVX512 = (cpuInfo[1] & (1 << 16)) != 0;
    hasSHA = (cpuInfo[1] & (1 << 29)) != 0;
}

void PCScan::SaveToConfig(const std::wstring& iniPath) {
    std::wofstream configFile(iniPath);
    if (!configFile) {
        std::wcerr << L"Failed to open config file: " << iniPath << std::endl;
        return;
    }

    configFile << L"[CPU]" << std::endl;
    configFile << L"VENDOR=" << cpuVendor.c_str() << std::endl;
    configFile << L"USE_SSE2=" << hasSSE2 << std::endl;
    configFile << L"USE_SSE3=" << hasSSE3 << std::endl;
    configFile << L"USE_SSSE3=" << hasSSSE3 << std::endl;
    configFile << L"USE_SSE4_1=" << hasSSE4_1 << std::endl;
    configFile << L"USE_SSE4_2=" << hasSSE4_2 << std::endl;
    configFile << L"USE_AVX=" << hasAVX << std::endl;
    configFile << L"USE_AVX2=" << hasAVX2 << std::endl;
    configFile << L"USE_AVX512=" << hasAVX512 << std::endl;
    configFile << L"USE_FMA=" << hasFMA << std::endl;
    configFile << L"USE_AES_NI=" << hasAES_NI << std::endl;
    configFile << L"USE_SHA=" << hasSHA << std::endl;
    configFile << L"USE_POPCNT=" << hasPOPCNT << std::endl;

    configFile.close();
}




