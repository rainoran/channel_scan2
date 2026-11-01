#include "OSScan.h"
#include <iostream>
#include <locale>

void OSScan::DetectOS() {
#ifdef _WIN32
    osType = L"Windows";
    std::locale::global(std::locale("ja_JP.SJIS"));
    std::wcout.imbue(std::locale("ja_JP.SJIS"));
#elif __linux__
    osType = L"Linux";
    std::locale::global(std::locale("ja_JP.UTF-8"));
    std::wcout.imbue(std::locale("ja_JP.UTF-8"));
#elif __APPLE__
    osType = L"macOS";
    std::locale::global(std::locale("ja_JP.UTF-8"));
    std::wcout.imbue(std::locale("ja_JP.UTF-8"));
#else
    osType = L"Unknown";
#endif
    std::wcout << L"Detected OS: " << osType << std::endl;
}

std::wstring OSScan::GetOSType() const {
    return osType;
}
