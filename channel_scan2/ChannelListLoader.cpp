#include "ChannelListLoader.hpp"
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>

std::wstring ChannelListLoader::Trim(const std::wstring& str) {
    const wchar_t* ws = L" \t\n\r\f\v,";
    size_t start = str.find_first_not_of(ws);
    size_t end = str.find_last_not_of(ws);
    return (start == std::wstring::npos) ? L"" : str.substr(start, end - start + 1);
}

uint16_t ChannelListLoader::ToUInt16Safe(const std::wstring& str) {
    std::wstring trimmed = Trim(str);
    if (trimmed.empty()) {
        std::wcerr << L"[WARN] 空の文字列をServiceID/NetworkID/TSIDに変換しようとしました。\n";
        return 0;
    }

    try {
        size_t idx = 0;
        int value = std::stoi(trimmed, &idx, 10);
        if (idx != trimmed.length()) {
            std::wcerr << L"[WARN] 数値変換に不適切な文字列が含まれています: " << trimmed << L"\n";
            return 0;
        }

        if (value < 0 || value > 0xFFFF) {
            std::wcerr << L"[WARN] 値が uint16_t の範囲外です: " << value << L"\n";
            return 0;
        }

        return static_cast<uint16_t>(value);
    }
    catch (const std::invalid_argument& e) {
        (void)e; // 未使用警告を抑制
        std::wcerr << L"[ERROR] 数値変換失敗 (invalid_argument): " << trimmed << L"\n";
        return 0;
    }
    catch (const std::out_of_range& e) {
        (void)e; // 未使用警告を抑制
        std::wcerr << L"[ERROR] 数値が範囲外です (out_of_range): " << trimmed << L"\n";
        return 0;
    }
    catch (...) {
        std::wcerr << L"[ERROR] 予期せぬ例外が発生しました: " << trimmed << L"\n";
        return 0;
    }
}


bool ChannelListLoader::LoadFromINI(const std::wstring& filename) {
    if (!std::filesystem::exists(filename)) {
        std::wcout << L"[INFO] INIファイルが存在しません: " << filename << std::endl;

        if (std::filesystem::exists(L"channellist.ch")) {
            std::wcout << L"[INFO] channellist.ch を元にINIを生成します。\n";
            if (!ConvertFromTXT(L"channellist.ch", filename)) {
                std::wcerr << L"[ERROR] TXTからINIへの変換に失敗しました。\n";
                return false;
            }
        }
        else {
            std::wcout << L"[INFO] channellist.ch が存在しません。初期INIを作成します。\n";
            if (!CreateInitialINI(filename)) {
                std::wcerr << L"[ERROR] 初期INIの作成に失敗しました。\n";
                return false;
            }
        }
    }

    return LoadExistingINI(filename);
}

bool ChannelListLoader::CreateInitialINI(const std::wstring& filename) {
    std::wofstream newFile(filename);
    if (!newFile.is_open()) return false;

    newFile << L"[Channel_1]\n";
    newFile << L"ServiceID=1\n";
    newFile << L"NetworkID=0\n";
    newFile << L"TSID=0\n";
    newFile << L"ServiceName=TestChannel\n\n";
    newFile.close();
    return true;
}

bool ChannelListLoader::ConvertFromTXT(const std::wstring& txtPath, const std::wstring& iniPath) {
    std::wifstream txtFile(txtPath);
    std::wofstream iniFile(iniPath);
    if (!txtFile.is_open() || !iniFile.is_open()) return false;

    std::wstring line;
    int index = 1;

    while (std::getline(txtFile, line)) {
        if (line.find(L"Channel:") != std::wstring::npos) {
            size_t sidPos = line.find(L"SID:");
            size_t dbPos = line.find(L"dB:");

            if (sidPos == std::wstring::npos || dbPos == std::wstring::npos) continue;

            std::wstring name = Trim(line.substr(0, sidPos));
            std::wstring sid = Trim(line.substr(sidPos + 4, dbPos - sidPos - 4));

            iniFile << L"[Channel_" << index++ << L"]\n";
            iniFile << L"ServiceID=" << sid << L"\n";
            iniFile << L"NetworkID=0\nTSID=0\n";
            iniFile << L"ServiceName=" << name << L"\n\n";
        }
    }

    txtFile.close();
    iniFile.close();
    return true;
}

bool ChannelListLoader::LoadExistingINI(const std::wstring& filename) {
    std::wifstream file(filename);
    if (!file.is_open()) return false;

    std::wstring line;
    ChannelEntry currentEntry;

    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty()) continue;

        if (line.find(L"[Channel_") == 0) {
            if (currentEntry.serviceId != 0 || !currentEntry.serviceName.empty()) {
                m_channels.push_back(currentEntry);          
                currentEntry = ChannelEntry();
            }
        }
        else if (line.find(L"ServiceID=") == 0) {
            currentEntry.serviceId = ToUInt16Safe(Trim(line.substr(10)));
        }
        else if (line.find(L"NetworkID=") == 0) {
            currentEntry.networkId = ToUInt16Safe(Trim(line.substr(10)));
        }
        else if (line.find(L"TSID=") == 0) {
            currentEntry.tsid = ToUInt16Safe(Trim(line.substr(6)));
        }
        else if (line.find(L"ServiceName=") == 0) {
            currentEntry.serviceName = Trim(line.substr(12));
        }
    }

    if (currentEntry.serviceId != 0 || !currentEntry.serviceName.empty()) {
        m_channels.push_back(currentEntry);                   
    }

    file.close();
    return true;
}




bool ChannelListLoader::LoadFromChannelListCh(const std::wstring& filename)
{
    m_channels.clear();

    std::wifstream file(filename);
    if (!file.is_open()) {
        std::wcerr << L"[ERROR] channellist 読み込み失敗: " << filename << L"\n";
        return false;
    }

    std::wstring line;

    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty())
            continue;

        // ★ 「;」から始まる行はすべてスキップ（コメント・ヘッダ）
        if (!line.empty() && line[0] == L';')
            continue;

        std::wstringstream ss(line);
        std::wstring col;
        std::vector<std::wstring> cols;

        while (std::getline(ss, col, L',')) {
            cols.push_back(Trim(col));
        }

        // ヘッダどおり 12 列あることを前提
        if (cols.size() < 12) {
            std::wcerr << L"[WARN] 列数が足りない行をスキップ: " << line << L"\n";
            continue;
        }

        ChannelEntry entry;

        // 0: ServiceName
        entry.serviceName = cols[0];

        // 1: Space / 2: Channel
        try {
            entry.space = std::stoi(cols[1]);
            entry.channel = std::stoi(cols[2]);
        }
        catch (...) {
            entry.space = entry.channel = 0;
        }

        // 3: ServiceType
        entry.serviceType = ToUInt16Safe(cols[3]);

        // 4: ServiceID
        entry.serviceId = ToUInt16Safe(cols[4]);

        // 5: NetworkID / 6: TSID / 7: ONID
        entry.networkId = ToUInt16Safe(cols[5]);
        entry.tsid = ToUInt16Safe(cols[6]);
        entry.onid = ToUInt16Safe(cols[7]);

        // 8: FreeCA
        entry.freeCA = (ToUInt16Safe(cols[8]) != 0);

        // 9: Provider / 10: NetworkName
        entry.provider = cols[9];
        entry.networkName = cols[10];

        // 11: RemoteControlKeyID
        try {
            entry.remoteControlKeyId = std::stoi(cols[11]);
        }
        catch (...) {
            entry.remoteControlKeyId = -1;
        }

        m_channels.push_back(std::move(entry));
    }

    file.close();

    std::wcout << L"[INFO] channellist 読み込み完了: " << filename
        << L"  件数=" << m_channels.size() << L"\n";

    return !m_channels.empty();
}
