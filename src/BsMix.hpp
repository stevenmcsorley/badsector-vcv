#pragma once

#include <algorithm>

struct BsMixGains {
	float liveDry = 0.f;
	float bufferedDry = 0.f;
	float wet = 0.f;
};

// Wet playback necessarily uses the division that has just been captured.
// Cross the dry monitor onto that same division in the first 10% of MIX so
// useful blended settings do not combine live audio with audio one TIME
// period earlier. Fully dry remains a genuine zero-latency monitor.
inline BsMixGains bsMixGains(float mix) {
	mix = std::max(0.f, std::min(1.f, mix));
	float align = std::max(0.f, std::min(1.f, mix * 10.f));
	align = align * align * (3.f - 2.f * align);  // smoothstep
	float dry = 1.f - mix;
	BsMixGains out;
	out.liveDry = dry * (1.f - align);
	out.bufferedDry = dry * align;
	out.wet = mix;
	return out;
}
