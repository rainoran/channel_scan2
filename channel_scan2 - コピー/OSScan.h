
#pragma once
#ifndef OSSCAN_H
#define OSSCAN_H

#include <string>

class OSScan {
public:
    void DetectOS();
    std::wstring GetOSType() const;

private:
    std::wstring osType;
};

#endif // OSSCAN_H
