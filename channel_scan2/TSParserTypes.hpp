
#pragma once
#ifndef TS_PARSER_TYPES_HPP
#define TS_PARSER_TYPES_HPP

#include <cstdint>
#include <vector>

struct PMTStreamInfo
{
    uint8_t stream_type;
    uint16_t elementary_PID;
    uint16_t ES_info_length;
    std::vector<uint8_t> descriptor_data;
};

#endif // TS_PARSER_TYPES_HPP
