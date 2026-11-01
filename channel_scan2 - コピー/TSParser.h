#pragma once
#ifndef TSPARSER_H
#define TSPARSER_H

#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>
#include <mutex> 
#include <shared_mutex>
#include "TSBuffer.h"
#include "TSBufferConstants.hpp"     // TS_BUFFER_INITIAL_SIZE などの定数をここから読み込む
#include "ITSFilter.hpp"         // すべてのフィルターはこのインターフェースを実装している前提

class TSParser {
public:
    explicit TSParser(size_t bufferSize = TS_BUFFER_INITIAL_SIZE);

    // TSデータをパーサーに追加
    void ParseTS(const uint8_t* data, size_t size);

    // 内部バッファからパケットを各フィルターに分配
    void DistributePackets();

    // フィルター登録（PID単位）
    void RegisterFilter(uint16_t pid, ITSFilter* filter);

    // 内部TSバッファ取得（TsReceiver等から）
    TSBuffer& GetTSBuffer();

private:
    std::unordered_map<uint16_t, ITSFilter*> filterMap;
    TSBuffer tsBuffer;
    std::shared_mutex parserMutex;

    // TSパケット一つをフィルターに配信
    void DispatchPacket(const uint8_t* packet, size_t size);
};

#endif // TSPARSER_H
