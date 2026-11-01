
#pragma once
#ifndef PCSCAN_H
#define PCSCAN_H

#include <string>
#include <unordered_map>

class PCScan {
public:
    void DetectCPU();
    void DetectMemory();
    void DetectGPU();
    void DetectStorage();
    void DetectCPUFeatures();
    void SaveToConfig(const std::wstring& iniPath);

    static PCScan& GetInstance();  

    bool IsFeatureEnabled(const std::string& feature) const;

private:
    std::string cpuVendor;
    bool hasSSE2 = false;
    bool hasSSE3 = false;
    bool hasSSSE3 = false;
    bool hasSSE4_1 = false;
    bool hasSSE4_2 = false;
    bool hasAVX = false;
    bool hasAVX2 = false;
    bool hasAVX512 = false;
    bool hasFMA = false;
    bool hasAES_NI = false;
    bool hasSHA = false;
    bool hasPOPCNT = false;
    unsigned long long totalRAM = 0;
    unsigned long long availableRAM = 0;
    std::string gpuVendor;
    bool hasNvidiaGPU = false;
    bool hasAmdGPU = false;
    bool hasIntelGPU = false;
    unsigned long long freeStorageGB = 0;

    PCScan();
};

#endif // PCSCAN_H
