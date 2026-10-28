#pragma once
#ifndef CRC32_HPP
#define CRC32_HPP

#include <cstdint>
#include <cstddef>


class CRC32
{
public:
    static uint32_t Calculate(const uint8_t* data, size_t length);
};

#endif
