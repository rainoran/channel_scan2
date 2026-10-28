#pragma once
#ifndef PACKET_DUMP_MANAGER_HPP
#define PACKET_DUMP_MANAGER_HPP


#include <vector>
#include <array>
#include <string>

class PacketDumpManager {
public:
    static void DumpHexPackets(const std::vector<std::array<uint8_t, 188>>& packets, const std::wstring& filename);

    // ✅ 追加（追記モード対応）
    static void DumpHexPackets(const std::vector<std::array<uint8_t, 188>>& packets, const std::wstring& filename, bool append);

    // ✅ 追加（1パケット即時出力用）
    static void DumpPacketHex(const std::array<uint8_t, 188>& pkt, const std::wstring& filename, bool append = true);

    // ラベル付き出力（1パケット）
    static void DumpPacketHexWithLabel(const std::array<uint8_t, 188>& pkt, const std::wstring& filename, const std::wstring& label, bool append = true);
    // 任意のバッファ出力用（ラベル付き）
    static void DumpRawBytesWithLabel(const uint8_t* data,size_t size,const std::wstring& filename,const std::wstring& label,bool append,bool fixedSize,uint8_t paddingValue);

	// バッファ全体をダンプ（ラベル付き）
    static void FlushDumpBuffer();
};

#endif // 