#include "PacketDumpManager.hpp"
#include "PacketDumpRingBuffer.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <Windows.h>
#include <format>

namespace {
    std::string ConvertWStringToUtf8(const std::wstring& wstr) {
        if (wstr.empty()) return {};

        int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string result(sizeNeeded, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), sizeNeeded, nullptr, nullptr);
        return result;
    }
}

/*void PacketDumpManager::DumpHexPackets(const std::vector<std::array<uint8_t, 188>>& packets, const std::wstring& filename)
{
    std::string utf8Filename = ConvertWStringToUtf8(filename);

    std::ofstream ofs(utf8Filename, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) return;

    for (const auto& pkt : packets) {
        std::ostringstream line;
        for (uint8_t b : pkt) {
            line << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(b) << ' ';
        }
        ofs << line.str() << '\n';
    }

    ofs.close();
}
*/
void PacketDumpManager::DumpHexPackets(const std::vector<std::array<uint8_t, 188>>& packets, const std::wstring& filename)
{
    DumpHexPackets(packets, filename, false);  // デフォルトは上書き
}

void PacketDumpManager::DumpHexPackets(const std::vector<std::array<uint8_t, 188>>& packets, const std::wstring& filename, bool append)
{
    std::wofstream ofs(filename, append ? std::ios::app : std::ios::trunc);
    if (!ofs)
        return;

    for (const auto& pkt : packets)
    {
        for (size_t i = 0; i < pkt.size(); ++i)
        {
            ofs << std::format(L"{:02X}", pkt[i]);
            if (i != pkt.size() - 1)
                ofs << L' ';  // 最後のスペースを除く
        }
        ofs << L"\n";  // パケットごとに改行
    }
}


void PacketDumpManager::DumpPacketHex(const std::array<uint8_t, 188>& pkt, const std::wstring& filename, bool append)
{
    DumpHexPackets(std::vector<std::array<uint8_t, 188>>{ pkt }, filename, append);
}

void PacketDumpManager::DumpPacketHexWithLabel(const std::array<uint8_t, 188>& pkt, const std::wstring& filename, const std::wstring& label, bool append)
{
    std::wofstream ofs(filename, append ? std::ios::app : std::ios::trunc);
    if (!ofs)
        return;

    ofs << L"[PACKET] " << label << L"\n";

    for (size_t i = 0; i < pkt.size(); ++i)
    {
        ofs << std::format(L"{:02X}", pkt[i]);
        if (i != pkt.size() - 1)
            ofs << L' ';
    }
    ofs << L"\n\n";
}

void PacketDumpManager::DumpRawBytesWithLabel(
    const uint8_t* data,
    size_t size,
    const std::wstring& filename,
    const std::wstring& label,
    bool append,
    bool fixedSize,
    uint8_t paddingValue)
{
    PacketDumpEntry entry;
    entry.data.assign(data, data + size);
    entry.filename = filename;
    entry.label = label;
    entry.append = append;
    entry.fixedSize = fixedSize;
    entry.paddingValue = paddingValue;

    PacketDumpRingBuffer::Push(entry);

    if (PacketDumpRingBuffer::IsOverThreshold()) {
        FlushDumpBuffer();
    }
}

void PacketDumpManager::FlushDumpBuffer()
{
    auto entries = PacketDumpRingBuffer::PopAll();

    for (const auto& entry : entries)
    {
        std::wofstream ofs(entry.filename, entry.append ? std::ios::app : std::ios::trunc);
        if (!ofs) continue;

        ofs << L"[RAW] " << entry.label << L"\n";

        size_t outputSize = entry.fixedSize ? 188 : entry.data.size();
        for (size_t i = 0; i < outputSize; ++i)
        {
            uint8_t val = (i < entry.data.size()) ? entry.data[i] : entry.paddingValue;
            ofs << std::format(L"{:02X}", val);
            if (i != outputSize - 1)
                ofs << L' ';
        }
        ofs << L"\n\n";
    }
}