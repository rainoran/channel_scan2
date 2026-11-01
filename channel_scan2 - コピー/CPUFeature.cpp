#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CPUFeature.h"

#include <intrin.h>
#include <cstring>
#include <thread>

namespace
{
    void CpuId_(
        int out[4],
        int leaf,
        int subLeaf = 0) noexcept
    {
        __cpuidex(out, leaf, subLeaf);
    }

    int GetMaxBasicLeaf_() noexcept
    {
        int cpuInfo[4]{};
        CpuId_(cpuInfo, 0, 0);
        return cpuInfo[0];
    }

    unsigned long long GetXcr0_() noexcept
    {
#if defined(_M_X64) || defined(_M_IX86)
        return _xgetbv(0);
#else
        return 0;
#endif
    }

    bool IsAvxOsEnabled_(
        bool cpuAvx,
        bool cpuXsave,
        bool cpuOsXsave) noexcept
    {
        if (!cpuAvx || !cpuXsave || !cpuOsXsave) {
            return false;
        }

        const unsigned long long xcr0 = GetXcr0_();

        // XMM bit1 + YMM bit2
        return (xcr0 & 0x6ull) == 0x6ull;
    }

    bool IsAvx512OsEnabled_(
        bool avxOsEnabled) noexcept
    {
        if (!avxOsEnabled) {
            return false;
        }

        const unsigned long long xcr0 = GetXcr0_();

        // bit1 XMM
        // bit2 YMM
        // bit5 opmask
        // bit6 ZMM_hi256
        // bit7 hi16_ZMM
        return (xcr0 & 0xE6ull) == 0xE6ull;
    }

    std::wstring MakeVendorString_() noexcept
    {
        int cpuInfo[4]{};
        CpuId_(cpuInfo, 0, 0);

        char vendor[13]{};

        std::memcpy(vendor + 0, &cpuInfo[1], 4); // EBX
        std::memcpy(vendor + 4, &cpuInfo[3], 4); // EDX
        std::memcpy(vendor + 8, &cpuInfo[2], 4); // ECX
        vendor[12] = '\0';

        std::wstring out;

        for (int i = 0; i < 12 && vendor[i] != '\0'; ++i) {
            out.push_back(static_cast<wchar_t>(
                static_cast<unsigned char>(vendor[i])));
        }

        return out;
    }
}

CPUFeature::CPUFeature() noexcept
{
    DetectCPUFeatures();
}

CPUFeature& CPUFeature::GetInstance() noexcept
{
    static CPUFeature instance;
    return instance;
}

void CPUFeature::DetectCPUFeatures() noexcept
{
    hasSSE2 = false;
    hasSSE3 = false;
    hasSSSE3 = false;
    hasSSE4_1 = false;
    hasSSE4_2 = false;

    hasAVX = false;
    hasAVX2 = false;
    hasAVX512 = false;

    hasFMA = false;
    hasPOPCNT = false;
    hasAESNI = false;
    hasSHA = false;
    hasBMI1 = false;
    hasBMI2 = false;

    vendorString_ = MakeVendorString_();

    const int maxBasicLeaf = GetMaxBasicLeaf_();
    
    {
        const unsigned int hc =
            std::thread::hardware_concurrency();

        logicalCoreCount_ =
            hc == 0u
            ? 1u
            : static_cast<std::uint32_t>(hc);
    }

    bool avxOsEnabled = false;

    if (maxBasicLeaf >= 1) {
        int cpuInfo[4]{};
        CpuId_(cpuInfo, 1, 0);

        const int ecx = cpuInfo[2];
        const int edx = cpuInfo[3];

        hasSSE2 = (edx & (1 << 26)) != 0;
        hasSSE3 = (ecx & (1 << 0)) != 0;
        hasSSSE3 = (ecx & (1 << 9)) != 0;
        hasSSE4_1 = (ecx & (1 << 19)) != 0;
        hasSSE4_2 = (ecx & (1 << 20)) != 0;

        hasPOPCNT = (ecx & (1 << 23)) != 0;
        hasAESNI = (ecx & (1 << 25)) != 0;

        const bool cpuFma =
            (ecx & (1 << 12)) != 0;

        const bool cpuXsave =
            (ecx & (1 << 26)) != 0;

        const bool cpuOsXsave =
            (ecx & (1 << 27)) != 0;

        const bool cpuAvx =
            (ecx & (1 << 28)) != 0;

        avxOsEnabled =
            IsAvxOsEnabled_(
                cpuAvx,
                cpuXsave,
                cpuOsXsave);

        hasAVX = avxOsEnabled;
        hasFMA = cpuFma && avxOsEnabled;
    }

    if (maxBasicLeaf >= 7) {
        int cpuInfo[4]{};
        CpuId_(cpuInfo, 7, 0);

        const int ebx = cpuInfo[1];

        const bool cpuAvx2 =
            (ebx & (1 << 5)) != 0;

        const bool cpuAvx512f =
            (ebx & (1 << 16)) != 0;

        hasBMI1 = (ebx & (1 << 3)) != 0;
        hasBMI2 = (ebx & (1 << 8)) != 0;
        hasSHA = (ebx & (1 << 29)) != 0;

        hasAVX2 =
            cpuAvx2 &&
            avxOsEnabled;

        hasAVX512 =
            cpuAvx512f &&
            IsAvx512OsEnabled_(avxOsEnabled);
    }
}

bool CPUFeature::IsIntel() const noexcept
{
    return vendorString_ == L"GenuineIntel";
}

bool CPUFeature::IsAMD() const noexcept
{
    return vendorString_ == L"AuthenticAMD";
}

const std::wstring& CPUFeature::GetVendorString() const noexcept
{
    return vendorString_;
}

bool CPUFeature::HasSSE2() const noexcept
{
    return hasSSE2;
}

bool CPUFeature::HasSSE42() const noexcept
{
    return hasSSE4_2;
}

bool CPUFeature::HasAVX() const noexcept
{
    return hasAVX;
}

bool CPUFeature::HasAVX2() const noexcept
{
    return hasAVX2;
}

bool CPUFeature::HasAVX512() const noexcept
{
    return hasAVX512;
}

std::uint32_t CPUFeature::GetLogicalCoreCount() const noexcept
{
    return logicalCoreCount_ == 0u ? 1u : logicalCoreCount_;
}