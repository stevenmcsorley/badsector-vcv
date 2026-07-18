#pragma once
#include <cstdint>

// Exact clock-phase repeat grid, shared by the module and tests/timing_test.
// Window k of `subs` spans [ceil(k*len/subs), ceil((k+1)*len/subs)) in whole
// samples: exactly `subs` windows tile a division of `len` samples, and every
// boundary sits within one sample of the ideal rational fraction k*len/subs.
// (A floored fixed window length accumulates early drift and fires an extra
// truncated retrigger whenever len is not divisible by subs.)

inline int bsGridIndex(int t, int len, int subs) {
	if (len < 1 || subs < 1 || t < 0) return 0;
	return (int)(((int64_t) t * subs) / len);
}

inline int bsGridStart(int k, int len, int subs) {
	if (len < 1 || subs < 1 || k < 0) return 0;
	return (int)(((int64_t) k * len + subs - 1) / subs);
}
