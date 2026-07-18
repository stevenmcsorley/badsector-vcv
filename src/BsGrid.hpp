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
	// A late/jittered clock edge can leave playback running beyond the
	// acquired division. Hold the final window instead of inventing windows
	// `subs`, `subs + 1`, ... while waiting for the next authoritative tick.
	if (t >= len) return subs - 1;
	return (int)(((int64_t) t * subs) / len);
}

inline int bsGridStart(int k, int len, int subs) {
	if (len < 1 || subs < 1 || k < 0) return 0;
	if (k > subs) k = subs;
	return (int)(((int64_t) k * len + subs - 1) / subs);
}

// While a Time change lengthens the current clock cycle, keep replaying the
// last acquired division instead of parking its window envelope at zero until
// the later boundary arrives. The new section is still acquired only on that
// authoritative boundary.
inline int bsGridPlaybackTime(int t, int len) {
	if (len < 1 || t < 0) return 0;
	return t % len;
}

// Advance the live repeat grid. A pending repeat-count change is applied only
// when the currently active grid reaches a boundary; the index is then
// recomputed on the new grid in the same sample. Keeping this state transition
// here means the DSP and the regression tests exercise identical logic.
inline bool bsGridAdvance(int t, int len, int targetSubs,
		int& activeSubs, int& lastWin, int& winIdx) {
	if (targetSubs < 1) targetSubs = 1;
	if (activeSubs < 1) activeSubs = targetSubs;

	winIdx = bsGridIndex(t, len, activeSubs);
	if (winIdx == lastWin)
		return false;

	if (targetSubs != activeSubs) {
		activeSubs = targetSubs;
		winIdx = bsGridIndex(t, len, activeSubs);
	}
	lastWin = winIdx;
	return true;
}
