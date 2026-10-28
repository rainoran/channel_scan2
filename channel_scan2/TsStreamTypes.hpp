
#pragma once
#ifndef TS_STREAM_TYPES_HPP
#define TS_STREAM_TYPES_HPP

#include <vector>
#include <array>
#include <cstdint>
#include "TSBufferConstants.hpp"

// 従来型（旧 ReceiveTsStreamBlock 用）
using TsStreamBlock = std::vector<uint8_t>;

// ★ 新型（PIDテスト・FilterManager と完全一致させる）
using TsPacket = std::array<uint8_t, TS_PACKET_SIZE>;

#endif // TS_STREAM_TYPES_HPP
