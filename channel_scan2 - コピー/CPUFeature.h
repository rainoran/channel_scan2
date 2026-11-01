#pragma once
#ifndef CPU_FEATURE_H
#define CPU_FEATURE_H

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <cstdint>

#include "CPUFeature.h"

class CPUFeature
{
public:
    static CPUFeature& GetInstance() noexcept;

    CPUFeature(const CPUFeature&) = delete;
    CPUFeature& operator=(const CPUFeature&) = delete;

    std::uint32_t GetLogicalCoreCount() const noexcept;

    void DetectCPUFeatures() noexcept;

    bool IsIntel() const noexcept;
    bool IsAMD() const noexcept;

    const std::wstring& GetVendorString() const noexcept;

    bool HasSSE2() const noexcept;
    bool HasSSE42() const noexcept;
    bool HasAVX() const noexcept;
    bool HasAVX2() const noexcept;
    bool HasAVX512() const noexcept;

    // 既存コード互換用に public のまま残す。
    bool hasSSE2 = false;
    bool hasSSE3 = false;
    bool hasSSSE3 = false;
    bool hasSSE4_1 = false;
    bool hasSSE4_2 = false;

    bool hasAVX = false;
    bool hasAVX2 = false;
    bool hasAVX512 = false;

    bool hasFMA = false;
    bool hasPOPCNT = false;
    bool hasAESNI = false;
    bool hasSHA = false;
    bool hasBMI1 = false;
    bool hasBMI2 = false;

private:
    CPUFeature() noexcept;

    std::wstring vendorString_;

    std::uint32_t logicalCoreCount_ = 1;
};

#endif // CPU_FEATURE_H