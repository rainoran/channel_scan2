#include <Windows.h>  // DWORD, LPCTSTR の定義
#include <iostream>   // std::wcout, std::cerr など
#include "IBonDriver2.h"  // IBonDriver2 の定義
#include "channel_scan.h"

#include <fstream>

void ScanChannels(IBonDriver2* bonDriver, const std::wstring& outputPath) {
    if (!bonDriver) {
        std::wcerr << L"BonDriver instance is null!" << std::endl;
        return;
    }

    std::wofstream outFile(outputPath);
    if (!outFile) {
        std::wcerr << L"Failed to open output file: " << outputPath << std::endl;
        return;
    }

    outFile << L"Tuner: " << bonDriver->GetTunerName() << std::endl;

    for (DWORD space = 0; ; ++space) {
        LPCTSTR spaceName = bonDriver->EnumTuningSpace(space);
        if (!spaceName) break;

        outFile << L"Space: " << spaceName << std::endl;
        std::wcout << L"Scanning Space: " << spaceName << std::endl;

        for (DWORD ch = 0; ; ++ch) {
            LPCTSTR channelName = bonDriver->EnumChannelName(space, ch);
            if (!channelName) {
                std::wcout << L"  No more channels in space " << space << std::endl;
                break;
            }
            std::wcout << L"  Scanning Channel: " << channelName << std::endl;

            // チャンネルをセット
            if (!bonDriver->SetChannel(space, ch)) {
                std::wcerr << L"Failed to set channel " << ch << " in space " << space << std::endl;
                continue;
            }
            // 信号レベルを取得
            float signalLevel = bonDriver->GetSignalLevel();
            std::wcout << L"  Signal Level: " << signalLevel << L" dB" << std::endl;

            // 信号レベルを取得
            
            if (signalLevel <= 0.0) {
                std::wcout << L"Skipping channel " << ch << " (Signal too weak: " << signalLevel << " dB)" << std::endl;
                continue;
            }

            // サービス ID の取得
            DWORD serviceID = bonDriver->GetCurChannel();

            outFile << L"  Channel: " << channelName
                << L", SID: " << serviceID
                << L", dB: " << signalLevel << std::endl;
        }
    }

    outFile.close();
    std::wcout << L"Scan completed. Results saved to " << outputPath << std::endl;
}

