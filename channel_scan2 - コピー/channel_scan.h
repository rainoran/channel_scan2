#pragma once
#ifndef CHANNEL_SCAN_H
#define CHANNEL_SCAN_H

#include "IBonDriver2.h"
#include <string>

void ScanChannels(IBonDriver2* bonDriver, const std::wstring& outputPath);

#endif // CHANNEL_SCAN_H
