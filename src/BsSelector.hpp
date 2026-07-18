#pragma once
#include <cmath>

// Three independently-stored values edited through one physical knob.
// A selector button cycles the active channel; the knob uses soft
// takeover (pickup): after switching, the knob must sweep across the
// stored value before it engages, so switching never causes a jump.
struct BsSelector {
	float vals[3] = {0.f, 0.f, 0.f};
	int sel = 0;
	bool caught = true;
	float lastKnob = 0.f;

	void reset(float a, float b, float c) {
		vals[0] = a; vals[1] = b; vals[2] = c;
		sel = 0;
		caught = true;
	}

	// selector button pressed: next channel, disengage the knob
	void advance(float knob) {
		sel = (sel + 1) % 3;
		caught = false;
		lastKnob = knob;
	}

	// after a patch/preset load the knob may sit anywhere: never jump
	void detach(float knob) {
		caught = false;
		lastKnob = knob;
	}

	// call every sample with the physical knob position; returns the
	// active channel's value
	float track(float knob) {
		if (!caught) {
			bool crossedUp = lastKnob <= vals[sel] && knob >= vals[sel];
			bool crossedDown = lastKnob >= vals[sel] && knob <= vals[sel];
			if (crossedUp || crossedDown || std::fabs(knob - vals[sel]) < 0.005f)
				caught = true;
		}
		if (caught)
			vals[sel] = knob;
		lastKnob = knob;
		return vals[sel];
	}
};
