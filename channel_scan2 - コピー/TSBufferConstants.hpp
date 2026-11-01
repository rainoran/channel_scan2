
#pragma once
#ifndef TS_CONSTANTS_HPP
#define TS_CONSTANTS_HPP

constexpr size_t TS_BUFFER_INITIAL_SIZE = 8 * 1024 * 1024; // 8MB
constexpr size_t TS_BUFFER_MAX_SIZE = 64 * 1024 * 1024; // 最大64MB
constexpr size_t TS_PACKET_SIZE = 188;
constexpr size_t TS_SYNC_BYTE = 0x47;
constexpr size_t TS_MAX_PACKET_SIZE = 204;
constexpr size_t TS_BUFFER_SIZE = 1024 * 1024 * 32; // 32MB
constexpr size_t TS_BUFFER_SWITCH_THRESHOLD = TS_BUFFER_SIZE * 9 / 10; // 90%
constexpr size_t TS_RING_BUFFER_DEFAULT_CAPACITY = 1024;
#endif // TS_CONSTANTS_HPP
