#pragma once
#pragma once
#ifndef BUFFER_CONSTANTS_H
#define BUFFER_CONSTANTS_H

// バッファサイズを固定で管理
constexpr size_t TS_PARSER_BUFFER_SIZE = 32 * 1024 * 1024;      // 32MB
constexpr size_t TS_RECEIVER_BUFFER_SIZE = 16 * 1024 * 1024;    // 16MB
constexpr size_t TS_BUFFER_MANAGER_SIZE = 64 * 1024 * 1024;     // 64MB

#endif // BUFFER_CONSTANTS_H
