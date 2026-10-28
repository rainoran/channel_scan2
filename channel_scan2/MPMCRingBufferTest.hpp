#pragma once
#ifndef MPMC_RING_BUFFER_TEST_HPP
#define MPMC_RING_BUFFER_TEST_HPP

#pragma once
#include <windows.h>
#include "IBonDriver2.h"

void RunMPMCRingBufferTest(IBonDriver2* bon);

void RunMPMCRingBufferWithPIDTest(IBonDriver2* bon);

#endif // MPMC_RING_BUFFER_TEST_HPP
