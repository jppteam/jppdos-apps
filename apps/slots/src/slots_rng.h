#pragma once

#include <stdint.h>

void     slots_rng_seed(uint32_t seed);
uint32_t slots_rng(uint32_t bound);   /* uniform [0, bound); bound 0 = raw u32 */
