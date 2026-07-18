// Offline tests for the DAMAGE / CV AMT selector state machine:
// stored values, cycling, soft takeover, and restore behaviour.
// Build & run:  g++ -std=c++11 -I../src selector_test.cpp -o sel_test && ./sel_test
#include "../src/BsSelector.hpp"
#include <cstdio>
#include <cmath>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main() {
	// 1. knob edits the active channel directly when caught
	BsSelector s;
	s.reset(0.2f, 0.5f, 0.8f);
	s.caught = true; s.lastKnob = 0.2f;
	CHECK(std::fabs(s.track(0.3f) - 0.3f) < 1e-6f, "caught knob edits channel 0");
	CHECK(std::fabs(s.vals[0] - 0.3f) < 1e-6f, "value stored");

	// 2. advancing cycles 0->1->2->0
	s.advance(0.3f);
	CHECK(s.sel == 1, "advance to channel 1");
	s.advance(0.3f); s.advance(0.3f);
	CHECK(s.sel == 0, "cycles back to 0");

	// 3. switching must NOT jump the new channel to the knob position
	s.reset(0.2f, 0.9f, 0.5f);
	s.caught = true; s.lastKnob = 0.2f;
	s.advance(0.2f);                       // now editing channel 1 (0.9)
	CHECK(std::fabs(s.track(0.2f) - 0.9f) < 1e-6f, "no jump on switch");
	CHECK(std::fabs(s.track(0.4f) - 0.9f) < 1e-6f, "still parked before pickup");

	// 4. pickup engages when the knob crosses the stored value
	CHECK(std::fabs(s.track(0.95f) - 0.95f) < 1e-6f, "picked up after crossing");
	CHECK(s.caught, "caught after crossing");
	CHECK(std::fabs(s.vals[1] - 0.95f) < 1e-6f, "channel 1 follows");

	// 5. crossing downward also engages
	s.reset(0.5f, 0.3f, 0.5f);
	s.advance(0.8f);                       // editing ch1 (0.3), knob at 0.8
	s.track(0.6f);
	CHECK(!s.caught, "not yet");
	s.track(0.25f);
	CHECK(s.caught, "downward crossing engages");

	// 6. near-match engages (within 0.005)
	s.reset(0.5f, 0.7f, 0.5f);
	s.advance(0.6f);
	s.track(0.702f);
	CHECK(s.caught, "close-enough engages");

	// 7. restore: detach never jumps even if the knob sits elsewhere
	BsSelector r;
	r.reset(0.1f, 0.2f, 0.3f);
	r.sel = 2;
	r.detach(0.9f);                        // knob restored at 0.9, value 0.3
	CHECK(std::fabs(r.track(0.9f) - 0.3f) < 1e-6f, "restore keeps stored value");
	r.track(0.5f);
	r.track(0.29f);                        // crosses 0.3 downward
	CHECK(r.caught, "re-engages after restore sweep");

	// 8. untouched channels are never disturbed
	CHECK(std::fabs(r.vals[0] - 0.1f) < 1e-6f && std::fabs(r.vals[1] - 0.2f) < 1e-6f,
	      "inactive channels untouched");

	if (fails == 0) printf("ALL PASS (8 groups)\n");
	return fails ? 1 : 0;
}
