/**
 * @file        perf/frame_rate.h
 * @brief       Guest frame rate tracking, available in every build config.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

namespace rex::perf {

// Called once per guest swap (VdSwap) by the command processor.
void RecordGuestSwap();

// Total guest swaps since startup.
uint64_t GetGuestSwapCount();

}  // namespace rex::perf
