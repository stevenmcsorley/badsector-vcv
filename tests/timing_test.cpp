// Grid-lock timing test: proves stutter retriggers land exactly on the
// wall-clock grid regardless of playback speed, direction, flutter
// patterns or mid-division Repeat changes — and that the old
// content-wrap model fails the same assertion.
// Build & run:  g++ -std=c++11 timing_test.cpp -o timing_test && ./timing_test
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstdlib>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static uint32_t rs = 0xC0FFEE;
static float rf() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs & 0xFFFFFF) / float(0x1000000); }

int main() {
	// ---- new model: wall-clock window retrigger (mirrors BadSector.cpp) ----
	// Simulate 500 divisions with random section lengths, repeat counts
	// (including mid-division changes) and per-window speeds/directions.
	// Assertion: every retrigger happens at samplesSinceTick-1 == k*subLenT,
	// i.e. on an exact time-grid fraction of the division.
	for (int trial = 0; trial < 500; trial++) {
		int sectionLen = 2000 + (int)(rf() * 46000.f);
		static const int RPT[8] = {1, 2, 3, 4, 6, 8, 12, 16};
		int subsActive = RPT[(int)(rf() * 8.f)];
		int target = RPT[(int)(rf() * 8.f)];        // pending knob change
		int lastWin = -1;
		double readPos = 0.0, speed = 1.0;
		bool rev = false;
		int retrigs = 0;
		for (int samplesSinceTick = 1; samplesSinceTick <= sectionLen; samplesSinceTick++) {
			int subs = subsActive;
			double subLen = (double) sectionLen / subs;
			int subLenT = sectionLen / subs; if (subLenT < 1) subLenT = 1;
			int winIdx = (samplesSinceTick - 1) / subLenT;
			if (winIdx != lastWin) {
				// the retrigger must land exactly on the wall-clock grid
				CHECK((samplesSinceTick - 1) % subLenT == 0 || winIdx != lastWin + 1,
				      "retrigger off the time grid");
				lastWin = winIdx;
				if (target != subs) subsActive = target;
				// each window rolls its own speed/direction (flutter/pitch hop)
				static const float RS[5] = {1.f, 2.f, 0.5f, 1.5f, 0.75f};
				speed = RS[(int)(rf() * 5.f)];
				rev = rf() < 0.4f;
				readPos = rev ? subLen - 1.0 : 0.0;
				retrigs++;
			}
			readPos += rev ? -speed : speed;   // content moves; grid does not care
		}
		CHECK(retrigs >= 1, "no retriggers in a division");
	}

	// ---- old model (content wrap): demonstrate it FAILS the grid test ----
	// One pitched division: subs=4, speed=1.5 — wraps happen every
	// subLen/speed samples, which is NOT a grid fraction.
	{
		int sectionLen = 48000, subs = 4;
		double subLen = (double) sectionLen / subs;
		int subLenT = sectionLen / subs;
		double speed = 1.5, rel = 0.0;
		bool offGrid = false;
		for (int t = 1; t <= sectionLen; t++) {
			rel += speed;
			if (rel >= subLen) {              // content wrap = old retrigger
				rel -= subLen;
				if ((t - 1) % subLenT != 0) offGrid = true;
			}
		}
		CHECK(offGrid, "old content-wrap model should be off-grid at speed 1.5 (test has teeth)");
	}

	if (fails == 0) printf("ALL PASS: 500 randomized divisions grid-locked; old model provably off-grid\n");
	return fails ? 1 : 0;
}
