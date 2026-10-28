// PrintUtil.cpp
#include "PrintUtil.hpp"
#include <windows.h>
#include <iostream>

// UTF-8 → wide
static std::wstring Utf8ToWide(const std::string& utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len == 0) return L"[UTF-8変換失敗]";
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
    return wide;
}

// wide → Shift_JIS
static std::string WideToSjis(const std::wstring& wide) {
    int len = WideCharToMultiByte(932, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len == 0) return "[SJIS変換失敗]";
    std::string sjis(len, '\0');
    WideCharToMultiByte(932, 0, wide.c_str(), -1, &sjis[0], len, nullptr, nullptr);
    return sjis;
}

namespace PrintUtil {

    void Print(const std::string& utf8Text) {
        UINT cp = GetConsoleOutputCP();
        if (cp == CP_UTF8) {
            std::cout << utf8Text;
        }
        else if (cp == 932) { // Shift_JIS
            std::wstring wide = Utf8ToWide(utf8Text);
            std::string sjis = WideToSjis(wide);
            std::cout << sjis;
        }
        else {
            std::cout << "[未対応コードページ " << cp << "] " << utf8Text;
        }
    }

    void PrintLine(const std::string& utf8Text) {
        Print(utf8Text + "\n");
    }

} // namespace PrintUtil
