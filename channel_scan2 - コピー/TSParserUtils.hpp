#pragma once
#ifndef TS_PARSER_UTILS_HPP
#define TS_PARSER_UTILS_HPP

#include <cstdint>

// 16ビットの読み取り
inline uint16_t Read16(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0] << 8 | data[1]);
}

// 24ビット相当の読み取り（uint32_tで代用）
inline uint32_t Read24(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 16) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]));
}

// 32ビットの読み取り
inline uint32_t Read32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        (static_cast<uint32_t>(data[3]));
}

// PID 抽出（TSパケットの2-3バイト目から）
inline uint16_t ExtractPID(const uint8_t* ts_packet)
{
    return ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];
}

#endif // TS_PARSER_UTILS_HPP
