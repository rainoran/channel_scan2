#pragma once

// TsDetails.h

#ifndef TSDETAILS_H
#define TSDETAILS_H

#include <windows.h>
#include <tchar.h>

struct TsDetails {
    DWORD streamId;          // ストリームID
    DWORD pid;               // PID
    LPCTSTR streamType;      // ストリームタイプ（例えば、音声、映像など）
    BOOL isEnabled;          // ストリームの有効/無効
    LPCTSTR description;     // ストリームの説明

    TsDetails()
        : streamId(0), pid(0), streamType(nullptr), isEnabled(FALSE), description(nullptr) {
    }
};

#endif // TSDETAILS_H
