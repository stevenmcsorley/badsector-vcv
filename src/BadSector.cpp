// Data Bender — a circuit-bent stereo audio buffer, VCV recreation of the Qu-Bit Data Bender.
// Firmware source is not public and the binary is stripped, so this follows the v1.4.5 manual.
// Official v1.4.7 only disables a software filter on Rev-5 hardware and adds no control changes.
// VCV interaction difference: Shift is toggled on/off instead of requiring the button to be held.
//
// Structure the manual pins down and this implements:
//   Time     internal clock: a smooth 16 s .. 80 Hz; external clock: /16 /8 /4 /2 x1 x2 x3 x4 x8.
//            Sets the sample period — the rate a new buffer section is acquired. Audio outside the
//            current section is still written in the background, so old audio resurfaces when Time
//            changes. The buffer holds over a minute.
//   Repeats  divides the section into subsections, played back repeatedly.
//   Mode     Macro (blue) = Bend/Break are automated per clock division, with CUMULATIVE knob zones.
//            Micro (green) = Bend is a -3..+3 octave 1V/Oct speed control (button toggles reverse),
//            Break toggles Traverse (pick a subsection) vs Silence (a silence duty cycle to 90%).
//   Corrupt  end-of-chain, 5 effects in this order: Decimate, Dropout, Destroy, DJ Filter, Vinyl Sim.
//            Off when the knob is fully CCW or CV <= 0V.
//   Freeze   stops recording; if Mix is fully dry when engaged it snaps fully wet.
// There is no clock output on this module, and no reverse button — reverse lives on the Bend button.
#include "plugin.hpp"
#include <cmath>
#include <vector>

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// musical subdivision counts (powers of two + triplets) — everything that
// subdivides the clock picks from this table so stutters stay on the grid
static const int DB_RPT[20] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64,
                               96, 128, 192, 256, 384, 512, 768, 1024};

struct DbRng {
	uint32_t s = 0xC0DEBEEFu;
	void seed(uint32_t v) { s = v ? v : 1u; }
	uint32_t u32() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
	float f() { return (u32() & 0xFFFFFF) / float(0x1000000); }
	float bip() { return f() * 2.f - 1.f; }
};

// TPT state-variable filter (DJ Filter corrupt)
struct SVF {
	float ic1 = 0.f, ic2 = 0.f;
	void reset() { ic1 = ic2 = 0.f; }
	void process(float in, float g, float k, float& lp, float& hp) {
		float a1 = 1.f / (1.f + g * (g + k));
		float a2 = g * a1, a3 = g * a2;
		float v3 = in - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		lp = v2; hp = in - k * v1 - v2;
	}
};

struct BadSector : Module {
	// Param/input indices are kept in their original order so existing patches keep their cables.
	enum ParamId {
		TIME_PARAM, REPEATS_PARAM, MIX_PARAM, BEND_PARAM, BREAK_PARAM, CORRUPT_PARAM,
		MODE_PARAM, CORRUPTBTN_PARAM, FREEZE_PARAM, BENDBTN_PARAM, SHIFT_PARAM,
		BREAKBTN_PARAM, CLOCKBTN_PARAM, PARAMS_LEN
	};
	enum InputId {
		LEFT_INPUT, RIGHT_INPUT,
		TIME_INPUT, REPEATS_INPUT, BEND_INPUT, BREAK_INPUT, MIX_INPUT, CLOCK_INPUT,
		CORRUPT_INPUT, BENDGATE_INPUT, BREAKGATE_INPUT, CORRUPTGATE_INPUT, FREEZE_INPUT,
		INPUTS_LEN
	};
	enum OutputId { LEFT_OUTPUT, RIGHT_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		SHIFT_LIGHT_R, SHIFT_LIGHT_G, SHIFT_LIGHT_B,
		CLOCK_LIGHT_R, CLOCK_LIGHT_G, CLOCK_LIGHT_B,
		MODE_LIGHT_R, MODE_LIGHT_G, MODE_LIGHT_B,
		BEND_LIGHT_R, BEND_LIGHT_G, BEND_LIGHT_B,
		BREAK_LIGHT_R, BREAK_LIGHT_G, BREAK_LIGHT_B,
		CORRUPT_LIGHT_R, CORRUPT_LIGHT_G, CORRUPT_LIGHT_B,
		FREEZE_LIGHT_R, FREEZE_LIGHT_G, FREEZE_LIGHT_B,
		LIGHTS_LEN
	};

	// "over a minute of stereo audio"
	static constexpr float MAX_SECONDS = 64.f;
	std::vector<float> bufL, bufR;
	int bufLen = 0, writeHead = 0;

	// playback
	float readPos[2] = {0.f, 0.f};
	int sectionStart = 0, sectionLen = 4800;
	int curSub[2] = {0, 0};
	int samplesSinceTick = 0;          // exact division length in samples
	int subsActive[2] = {-1, -1};      // latched at stutter boundaries
	float lastPhase[2] = {0.f, 0.f};
	float speed[2] = {1.f, 1.f};
	float speedTarget[2] = {1.f, 1.f};
	float speedSlew[2] = {0.f, 0.f};
	bool revNow[2] = {false, false};

	// state
	int freezeHead = 0;         // writeHead captured when freeze engaged
	bool wasFreezeActive = false;
	bool macro = true;          // Macro mode is the default
	bool frozen = false, bendOn = false, breakOn = false;
	bool microRev = false;      // Micro: Bend button toggles reverse
	bool microSilence = false;  // Micro: Break button toggles Silence/Traverse (Traverse is default)
	int corruptSel = 0;         // Decimate, Dropout, Destroy, DJ Filter, Vinyl Sim
	float windowing = 0.02f;    // Shift+Time, default 2%
	float stereoWidth = 0.f;    // Shift+Mix
	bool stereoUnique = false;  // Shift+Bend button; manual default/restore = Shared
	bool freezeMixWet = false;  // freeze engaged while fully dry -> force wet

	// VCV adaptation: Shift is a latched edit mode rather than a button that must be held.
	bool shiftLatched = false;
	float ledBrightness = 1.f;
	float bendCvAtt = 1.f, breakCvAtt = 1.f, corruptCvAtt = 1.f;
	bool gatesMomentary = false;
	bool freezeMomentary = false;
	bool corruptAsReset = false;
	bool originalCorruptOnly = true;   // manual has exactly 3 corrupt effects
	bool freezeTogglePending = false;
	bool freezeButtonWasHigh = false;
	bool resetDivisionPending = false;

	// Secondary knob editing must not move the primary parameter. A primary resumes when
	// that knob is moved after leaving Shift, or when its CV jack is in use.
	enum ShiftKnob { SK_TIME, SK_REPEATS, SK_MIX, SK_BEND, SK_BREAK, SK_CORRUPT, SK_COUNT };
	float activeKnob[SK_COUNT] = {0.5f, 0.f, 0.5f, 0.f, 0.f, 0.f};
	float lastKnob[SK_COUNT] = {0.5f, 0.f, 0.5f, 0.f, 0.f, 0.f};
	bool shiftedKnob[SK_COUNT] = {};
	bool resumeKnobOnMove[SK_COUNT] = {};

	// clock
	bool extClock = false;
	float clkPhase = 0.f, extPeriod = 0.5f, sinceClk = 0.f;
	bool haveClk = false;
	int lastDiv = 4;
	int edgeCount = 0, multTick = 0;
	float divBlip = 0.f, clkBlink = 0.f, breakBlip = 0.f, restoreBlip = 0.f;

	// macro per-clock decisions (per channel when stereoUnique)
	float macroSpeed[2] = {1.f, 1.f};
	bool macroRev[2] = {false, false};
	float macroSilence[2] = {0.f, 0.f};
	float tapeStop[2] = {0.f, 0.f};
	float bendClick[2] = {0.f, 0.f};
	int breakSubs[2] = {0, 0};

	// corrupt state
	float decHoldL = 0.f, decHoldR = 0.f; int decCount = 0; int decVariant = 0;
	float dropEnv = 1.f; int dropTimer = 0;
	SVF djL, djR;
	float vinylPhase = 0.f;
	float vinylLpL = 0.f, vinylLpR = 0.f;
	float dcPrevInL = 0.f, dcPrevInR = 0.f, dcPrevOutL = 0.f, dcPrevOutR = 0.f;

	DbRng rng;
	dsp::BooleanTrigger shiftBtn, modeBtn, corruptBtn, bendBtn, breakBtn, clockBtn;
	dsp::SchmittTrigger clockTrig, bendGate, breakGate, corruptGate, freezeGate;

	BadSector() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TIME_PARAM, 0.f, 1.f, 0.5f, "Time (16s .. 80Hz, or clock div/mult)");
		configParam(REPEATS_PARAM, 0.f, 1.f, 0.f, "Repeats (1..1024, up into audio rate)");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix", "%", 0.f, 100.f);
		configParam(BEND_PARAM, 0.f, 1.f, 0.f, "Bend");
		configParam(BREAK_PARAM, 0.f, 1.f, 0.f, "Break");
		configParam(CORRUPT_PARAM, 0.f, 1.f, 0.f, "Corrupt (off when fully CCW)");
		configButton(MODE_PARAM, "Mode (Macro / Micro)");
		configButton(CORRUPTBTN_PARAM, "Corrupt effect (Decimate/Dropout/Destroy/DJ Filter/Vinyl Sim)");
		configButton(FREEZE_PARAM, "Freeze");
		configButton(BENDBTN_PARAM, "Bend on/off (Macro) / reverse (Micro)");
		configButton(BREAKBTN_PARAM, "Break on/off (Macro) / Traverse-Silence (Micro)");
		configButton(CLOCKBTN_PARAM, "Clock source (internal / external)");
		configButton(SHIFT_PARAM, "Shift edit mode (toggle)");
		configInput(LEFT_INPUT, "Left audio (normals to both channels)");
		configInput(RIGHT_INPUT, "Right audio");
		configInput(TIME_INPUT, "Time CV");
		configInput(REPEATS_INPUT, "Repeats CV");
		configInput(BEND_INPUT, "Bend CV (1V/Oct in Micro mode)");
		configInput(BREAK_INPUT, "Break CV");
		configInput(MIX_INPUT, "Mix CV");
		configInput(CLOCK_INPUT, "Clock");
		configInput(CORRUPT_INPUT, "Corrupt CV");
		configInput(BENDGATE_INPUT, "Bend gate");
		configInput(BREAKGATE_INPUT, "Break gate");
		configInput(CORRUPTGATE_INPUT, "Corrupt gate (advances effect)");
		configInput(FREEZE_INPUT, "Freeze gate");
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
		alloc(48000.f);
	}

	void alloc(float sr) {
		bufLen = (int)(sr * MAX_SECONDS);
		bufL.assign(bufLen, 0.f); bufR.assign(bufLen, 0.f);
		writeHead = 0; readPos[0] = readPos[1] = 0.f; sectionStart = 0;
	}
	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		if ((int)(e.sampleRate * MAX_SECONDS) != bufLen) alloc(e.sampleRate);
	}
	void restoreDefaults() {
		windowing = 0.02f; bendOn = false; breakOn = false; frozen = false;
		macro = true; stereoUnique = false; corruptSel = 0;   // manual: restore -> Shared stereo
		microRev = false; microSilence = false;
		gatesMomentary = false; freezeMomentary = false; corruptAsReset = false;
		freezeTogglePending = false; resetDivisionPending = false;
	}
	void onReset() override {
		std::fill(bufL.begin(), bufL.end(), 0.f); std::fill(bufR.begin(), bufR.end(), 0.f);
		writeHead = 0; readPos[0] = readPos[1] = 0.f; sectionStart = 0;
		curSub[0] = curSub[1] = 0;
		restoreDefaults(); extClock = false; stereoWidth = 0.f;
		freezeHead = 0; wasFreezeActive = false;
		shiftLatched = false; ledBrightness = 1.f;
		bendCvAtt = breakCvAtt = corruptCvAtt = 1.f;
		originalCorruptOnly = true; freezeButtonWasHigh = false;
		for (int i = 0; i < SK_COUNT; i++) {
			shiftedKnob[i] = resumeKnobOnMove[i] = false;
		}
		djL.reset(); djR.reset();
		vinylLpL = vinylLpR = 0.f;
		dcPrevInL = dcPrevInR = dcPrevOutL = dcPrevOutR = 0.f;
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "macro", json_boolean(macro));
		json_object_set_new(r, "frozen", json_boolean(frozen));
		json_object_set_new(r, "bendOn", json_boolean(bendOn));
		json_object_set_new(r, "breakOn", json_boolean(breakOn));
		json_object_set_new(r, "microRev", json_boolean(microRev));
		json_object_set_new(r, "microSilence", json_boolean(microSilence));
		json_object_set_new(r, "extClock", json_boolean(extClock));
		json_object_set_new(r, "stereoUnique", json_boolean(stereoUnique));
		json_object_set_new(r, "corruptSel", json_integer(corruptSel));
		json_object_set_new(r, "windowing", json_real(windowing));
		json_object_set_new(r, "stereoWidth", json_real(stereoWidth));
		json_object_set_new(r, "ledBrightness", json_real(ledBrightness));
		json_object_set_new(r, "bendCvAtt", json_real(bendCvAtt));
		json_object_set_new(r, "breakCvAtt", json_real(breakCvAtt));
		json_object_set_new(r, "corruptCvAtt", json_real(corruptCvAtt));
		json_object_set_new(r, "gatesMomentary", json_boolean(gatesMomentary));
		json_object_set_new(r, "freezeMomentary", json_boolean(freezeMomentary));
		json_object_set_new(r, "corruptAsReset", json_boolean(corruptAsReset));
		json_object_set_new(r, "originalCorruptOnly", json_boolean(originalCorruptOnly));
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "macro")) macro = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "frozen")) frozen = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "bendOn")) bendOn = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "breakOn")) breakOn = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "microRev")) microRev = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "microSilence")) microSilence = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "extClock")) extClock = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "stereoUnique")) stereoUnique = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "corruptSel")) corruptSel = clamp((int) json_integer_value(j), 0, 4);
		if (json_t* j = json_object_get(r, "windowing")) windowing = (float) json_real_value(j);
		if (json_t* j = json_object_get(r, "stereoWidth")) stereoWidth = (float) json_real_value(j);
		if (json_t* j = json_object_get(r, "ledBrightness")) ledBrightness = clampf((float) json_real_value(j), 0.05f, 1.f);
		if (json_t* j = json_object_get(r, "bendCvAtt")) bendCvAtt = clampf((float) json_real_value(j), 0.f, 1.f);
		if (json_t* j = json_object_get(r, "breakCvAtt")) breakCvAtt = clampf((float) json_real_value(j), 0.f, 1.f);
		if (json_t* j = json_object_get(r, "corruptCvAtt")) corruptCvAtt = clampf((float) json_real_value(j), 0.f, 1.f);
		if (json_t* j = json_object_get(r, "gatesMomentary")) gatesMomentary = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "freezeMomentary")) freezeMomentary = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "corruptAsReset")) corruptAsReset = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "originalCorruptOnly")) originalCorruptOnly = json_boolean_value(j);
		if (originalCorruptOnly && corruptSel >= 3) corruptSel = 0;
	}

	float readBuf(const std::vector<float>& b, float pos) {
		pos -= std::floor(pos / bufLen) * bufLen;
		int i0 = (int) pos; float fr = pos - i0;
		int i1 = i0 + 1; if (i1 >= bufLen) i1 = 0;
		return lerpf(b[i0], b[i1], fr);
	}

	// "Corrupt is an interchangeable end-of-chain effect ... off when the knob is fully CCW
	// and/or when <=0V is present at the CV input."
	void applyCorrupt(int effect, float amt, float& l, float& r, float sr) {
		if (amt <= 0.001f) return;
		switch (effect) {
			case 0: {  // Decimate — "variable amounts of bit-crushing and downsampling"
				// continuous per the manual: sample-hold length rises 1 -> ~55
				// samples while bit depth falls 16 -> 2 bits across the knob
				int hold = 1 + (int)(amt * amt * 54.f);
				float bits = 16.f - amt * 14.f;
				if (--decCount <= 0) { decCount = hold; decHoldL = l; decHoldR = r; }
				float q = std::pow(2.f, bits - 1.f);
				l = std::round(decHoldL * q) / q;
				r = std::round(decHoldR * q) / q;
			} break;
			case 1: {  // Dropout — left: fewer but longer; right: more but shorter
				if (--dropTimer <= 0) {
					float gapLen  = lerpf(0.35f, 0.02f, amt) * sr;     // shorter as the knob rises
					float betweenLen = lerpf(1.2f, 0.05f, amt) * sr;   // more often as it rises
					if (dropEnv < 0.5f) { dropEnv = 1.f; dropTimer = (int)(betweenLen * (0.5f + rng.f())); }
					else                { dropEnv = 0.f; dropTimer = (int)(gapLen * (0.5f + rng.f())); }
				}
				l *= dropEnv; r *= dropEnv;
			} break;
			case 2: {  // Destroy — first half soft saturation, second half hard clipping
				if (amt < 0.5f) {
					float d = 1.f + amt * 6.f;
					l = std::tanh(l * d) / std::sqrt(d);
					r = std::tanh(r * d) / std::sqrt(d);
				} else {
					float d = 1.f + (amt - 0.5f) * 60.f;
					l = clampf(l * d, -1.f, 1.f);
					r = clampf(r * d, -1.f, 1.f);
				}
			} break;
			case 3: {  // DJ Filter — no filtering at 12 o'clock, LP below, HP above
				float lp, hp, dummy;
				if (amt < 0.48f) {
					// closing as the knob goes down: 20kHz at centre -> 30Hz fully CCW
					float t = amt / 0.48f;
					float fc = 30.f * std::pow(666.f, t);
					float g = std::tan(M_PI * clampf(fc, 20.f, 18000.f) / sr);
					djL.process(l, g, 0.8f, lp, dummy); l = lp;
					djR.process(r, g, 0.8f, lp, dummy); r = lp;
				} else if (amt > 0.52f) {
					// opening as the knob goes up: 20Hz at centre -> 12kHz fully CW
					float t = (amt - 0.52f) / 0.48f;
					float fc = 20.f * std::pow(600.f, t);
					float g = std::tan(M_PI * clampf(fc, 20.f, 18000.f) / sr);
					djL.process(l, g, 0.8f, dummy, hp); l = hp;
					djR.process(r, g, 0.8f, dummy, hp); r = hp;
				}
				// 0.48..0.52 = 12 o'clock: no filtering
			} break;
			default: {  // Vinyl Sim — dust, pops and coloring
				if (rng.f() < amt * 0.0008f) {          // pops
					float c = rng.bip() * amt * 0.8f;
					l += c; r += c * 0.7f;
				}
				if (rng.f() < amt * 0.02f) {            // dust
					float c = rng.bip() * amt * 0.15f;
					l += c; r += c;
				}
				// Wow plus the progressively darker bandwidth of an old playback system.
				vinylPhase += 0.55f / sr; if (vinylPhase >= 1.f) vinylPhase -= 1.f;
				float fc = lerpf(18000.f, 3500.f, amt);
				float a = 1.f - std::exp(-2.f * (float) M_PI * fc / sr);
				vinylLpL += a * (l - vinylLpL);
				vinylLpR += a * (r - vinylLpR);
				float col = 1.f - amt * 0.15f;
				l = vinylLpL * (col + amt * 0.05f * std::sin(2.f * M_PI * vinylPhase));
				r = vinylLpR * (col + amt * 0.05f * std::sin(2.f * M_PI * vinylPhase + 0.7f));
			} break;
		}
	}

	// Every clock division, Macro mode rolls new manipulations. Both Bend and Break use
	// CUMULATIVE knob zones — "each variation is added to the ones before it".
	void rollMacro(float bendAmt, float breakAmt, int repeats, int repeatsIdx, bool bendEnabled, bool breakEnabled) {
		// ---- Bend: none -> Reverse -> Octaves -> 2 Octaves -> Tape Stop -> Slew -> Everything
		int nCh = stereoUnique ? 2 : 1;
		if (bendEnabled && bendAmt > 0.001f) {
			int z = (int) std::ceil(bendAmt * 6.f);
			float top = clampf(bendAmt * 6.f - (z - 1), 0.f, 1.f);
			auto za = [&](int k) { return (k < z) ? 1.f : (k == z ? top : 0.f); };
			// pitch changes come "in octaves and fifths, so it always sounds musical"
			static const float R2[4] = {2.f, 0.5f, 1.5f, 0.75f};        // octave / fifth
			static const float R3[4] = {4.f, 0.25f, 3.f, 1.f / 3.f};    // 2 oct / oct+fifth
			for (int c = 0; c < nCh; c++) {
				float sp = 1.f; bool rv = false;
				if (z >= 1 && rng.f() < za(1) * 0.5f) rv = true;                        // reverse
				if (z >= 2 && rng.f() < za(2) * 0.5f) sp *= R2[rng.u32() & 3];
				if (z >= 3 && rng.f() < za(3) * 0.4f) sp *= R3[rng.u32() & 3];
				if (z >= 4 && rng.f() < za(4) * 0.3f) tapeStop[c] = 1.f;                // tape stop
				bendClick[c] = (rng.f() < bendAmt * 0.35f) ? rng.bip() * bendAmt * 0.16f : 0.f;
				macroSpeed[c] = sp; macroRev[c] = rv;
				speedSlew[c] = (z >= 5) ? za(5) * 0.25f : 0.f;
			}
			if (!stereoUnique) {
				macroSpeed[1] = macroSpeed[0]; macroRev[1] = macroRev[0];
				tapeStop[1] = tapeStop[0]; bendClick[1] = bendClick[0]; speedSlew[1] = speedSlew[0];
			}
		} else {
			macroSpeed[0] = macroSpeed[1] = 1.f;
			macroRev[0] = macroRev[1] = false;
			speedSlew[0] = speedSlew[1] = 0.f;
			bendClick[0] = bendClick[1] = 0.f;
		}

		// ---- Break: none -> 2 Sub-sections -> Jumping -> More Subsections -> Audio Rate
		//              -> Silence -> Everything
		if (breakEnabled && breakAmt > 0.001f) {
			int z = (int) std::ceil(breakAmt * 6.f);
			float top = clampf(breakAmt * 6.f - (z - 1), 0.f, 1.f);
			auto za = [&](int k) { return (k < z) ? 1.f : (k == z ? top : 0.f); };
			for (int c = 0; c < nCh; c++) {
				// "set the repeats to anywhere ABOVE where the knob is set" —
				// always picked from the musical table so extra repeats stay
				// locked to the clock grid
				int subs = std::max(1, repeats);
				if (z >= 1 && rng.f() < za(1) * 0.5f) subs = std::max(subs, 2);
				if (z >= 3 && rng.f() < za(3) * 0.6f)
					subs = std::max(subs, DB_RPT[std::min(19, repeatsIdx + 1 + (int)(rng.f() * 4.f))]);
				if (z >= 4 && rng.f() < za(4) * 0.5f)
					subs = std::max(subs, DB_RPT[9 + (int)(rng.f() * 5.f)]);   // 32..192: audio rate
				if (z >= 2 && rng.f() < za(2) * 0.7f) curSub[c] = (int)(rng.f() * subs);
				macroSilence[c] = (z >= 5) ? za(5) * 0.9f * rng.f() : 0.f;
				breakSubs[c] = subs;
			}
			if (!stereoUnique) {
				curSub[1] = curSub[0]; macroSilence[1] = macroSilence[0]; breakSubs[1] = breakSubs[0];
			}
		} else {
			macroSilence[0] = macroSilence[1] = 0.f;
			breakSubs[0] = breakSubs[1] = 0;
		}
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime, sr = args.sampleRate;
		if (bufLen < 8) return;

		// ---- latched Shift + knob routing ----
		const int knobParams[SK_COUNT] = {
			TIME_PARAM, REPEATS_PARAM, MIX_PARAM, BEND_PARAM, BREAK_PARAM, CORRUPT_PARAM
		};
		const int knobInputs[SK_COUNT] = {
			TIME_INPUT, REPEATS_INPUT, MIX_INPUT, BEND_INPUT, BREAK_INPUT, CORRUPT_INPUT
		};
		float rawKnob[SK_COUNT];
		for (int i = 0; i < SK_COUNT; i++) rawKnob[i] = params[knobParams[i]].getValue();

		if (shiftBtn.process(params[SHIFT_PARAM].getValue() > 0.5f)) {
			shiftLatched = !shiftLatched;
			for (int i = 0; i < SK_COUNT; i++) {
				if (shiftLatched) shiftedKnob[i] = false;
				else resumeKnobOnMove[i] = shiftedKnob[i];
				lastKnob[i] = rawKnob[i];
			}
		}
		bool shift = shiftLatched;
		for (int i = 0; i < SK_COUNT; i++) {
			bool moved = std::fabs(rawKnob[i] - lastKnob[i]) > 1e-6f;
			if (shift) {
				if (moved) {
					shiftedKnob[i] = true;
					switch (i) {
						case SK_TIME: windowing = rawKnob[i]; break;
						case SK_REPEATS: ledBrightness = 0.05f + rawKnob[i] * 0.95f; break;
						case SK_MIX: stereoWidth = rawKnob[i]; break;
						case SK_BEND: bendCvAtt = rawKnob[i]; break;
						case SK_BREAK: breakCvAtt = rawKnob[i]; break;
						case SK_CORRUPT: corruptCvAtt = rawKnob[i]; break;
					}
				}
			} else if (!resumeKnobOnMove[i] || moved || inputs[knobInputs[i]].isConnected()) {
				activeKnob[i] = rawKnob[i];
				resumeKnobOnMove[i] = false;
			}
			lastKnob[i] = rawKnob[i];
		}

		// ---- primary knobs + CV ----
		float timeN = clampf(activeKnob[SK_TIME] + inputs[TIME_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float repeatsN = clampf(activeKnob[SK_REPEATS] + inputs[REPEATS_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float mixN = clampf(activeKnob[SK_MIX] + inputs[MIX_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float bendN = clampf(activeKnob[SK_BEND] + inputs[BEND_INPUT].getVoltage() * 0.1f * bendCvAtt, 0.f, 1.f);
		float breakN = clampf(activeKnob[SK_BREAK] + inputs[BREAK_INPUT].getVoltage() * 0.1f * breakCvAtt, 0.f, 1.f);
		float corruptN = activeKnob[SK_CORRUPT];
		if (inputs[CORRUPT_INPUT].isConnected()) {
			float cv = inputs[CORRUPT_INPUT].getVoltage();
			corruptN = (cv <= 0.f) ? 0.f : clampf(activeKnob[SK_CORRUPT] + cv * 0.1f * corruptCvAtt, 0.f, 1.f);
		}

		// ---- buttons + gates ----
		bool modePress = modeBtn.process(params[MODE_PARAM].getValue() > 0.5f);
		bool clockPress = clockBtn.process(params[CLOCKBTN_PARAM].getValue() > 0.5f);
		bool bendPress = bendBtn.process(params[BENDBTN_PARAM].getValue() > 0.5f);
		bool breakPress = breakBtn.process(params[BREAKBTN_PARAM].getValue() > 0.5f);
		bool corruptPress = corruptBtn.process(params[CORRUPTBTN_PARAM].getValue() > 0.5f);
		bool freezeButtonHigh = params[FREEZE_PARAM].getValue() > 0.5f;
		bool freezePress = !freezeButtonWasHigh && freezeButtonHigh;
		bool freezeRelease = freezeButtonWasHigh && !freezeButtonHigh;
		freezeButtonWasHigh = freezeButtonHigh;

		bool bendGateHigh = inputs[BENDGATE_INPUT].getVoltage() >= 0.4f;
		bool breakGateHigh = inputs[BREAKGATE_INPUT].getVoltage() >= 0.4f;
		bool corruptGateHigh = inputs[CORRUPTGATE_INPUT].getVoltage() >= 0.4f;
		bool freezeGateHigh = inputs[FREEZE_INPUT].getVoltage() >= 0.4f;
		bool bendGateEdge = bendGate.process(inputs[BENDGATE_INPUT].getVoltage(), 0.1f, 0.4f);
		bool breakGateEdge = breakGate.process(inputs[BREAKGATE_INPUT].getVoltage(), 0.1f, 0.4f);
		bool corruptGateEdge = corruptGate.process(inputs[CORRUPTGATE_INPUT].getVoltage(), 0.1f, 0.4f);
		bool freezeGateEdge = freezeGate.process(inputs[FREEZE_INPUT].getVoltage(), 0.1f, 0.4f);

		if (modePress) {
			if (shift) {
				originalCorruptOnly = !originalCorruptOnly;
				if (originalCorruptOnly && corruptSel >= 3) corruptSel = 0;
			} else macro = !macro;
		}
		if (clockPress) {
			if (shift) gatesMomentary = !gatesMomentary;
			else extClock = !extClock;
		}
		if (bendPress) {
			if (shift) stereoUnique = !stereoUnique;
			else if (macro) bendOn = !bendOn;
			else microRev = !microRev;
		}
		if (breakPress) {
			if (shift) { restoreDefaults(); restoreBlip = 0.5f; }
			else if (macro) breakOn = !breakOn;
			else microSilence = !microSilence;
		}
		if (corruptPress) {
			if (shift) corruptAsReset = !corruptAsReset;
			else corruptSel = (corruptSel + 1) % (originalCorruptOnly ? 3 : 5);
		}
		if (freezePress && shift) freezeMomentary = !freezeMomentary;
		if (freezeRelease && !shift && !freezeMomentary) freezeTogglePending = !freezeTogglePending;

		if (!gatesMomentary) {
			if (bendGateEdge) { if (macro) bendOn = !bendOn; else microRev = !microRev; }
			if (breakGateEdge) { if (macro) breakOn = !breakOn; else microSilence = !microSilence; }
			if (freezeGateEdge) freezeTogglePending = !freezeTogglePending;
			if (corruptGateEdge && !corruptAsReset)
				corruptSel = (corruptSel + 1) % (originalCorruptOnly ? 3 : 5);
		}
		bool bendEnabled = bendOn || (gatesMomentary && bendGateHigh);
		bool breakEnabled = breakOn || (gatesMomentary && breakGateHigh);
		bool reverseEnabled = microRev || (gatesMomentary && bendGateHigh);
		bool silenceEnabled = microSilence || (gatesMomentary && breakGateHigh);
		int corruptEffect = corruptSel;
		if (gatesMomentary && corruptGateHigh && !corruptAsReset)
			corruptEffect = (corruptEffect + 1) % (originalCorruptOnly ? 3 : 5);
		bool corruptResetEdge = corruptAsReset && corruptGateEdge;

		// ---- clock ----
		bool tick = false;
		float period;
		if (corruptResetEdge) {
			if (extClock) resetDivisionPending = true;
			else { clkPhase = 0.f; tick = true; }
		}
		if (extClock) {
			// Time is a div/mult of the incoming clock. Every division/x1 tick
			// lands ON an incoming edge, and multiplications phase-reset on
			// each edge — a free-running phase accumulator drifts off the beat.
			int d = clamp((int)(timeN * 8.99f), 0, 8);
			if (d != lastDiv) { lastDiv = d; divBlip = 0.35f; }
			sinceClk += dt;
			bool edge = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 0.4f);
			bool resetOnEdge = edge && resetDivisionPending;
			if (edge) {
				if (haveClk) extPeriod = clampf(sinceClk, 0.002f, 30.f);
				haveClk = true; sinceClk = 0.f;
				if (resetOnEdge) { edgeCount = 0; resetDivisionPending = false; }
				else edgeCount++;
			}
			// hard-lock to edges while the clock is present; if edges stop
			// arriving, free-run on the measured period (the hardware keeps
			// time too — that is its "dim white" state) so audio never dies
			bool lost = sinceClk > extPeriod * 1.1f;
			if (d <= 3) {                       // divisions /16 /8 /4 /2
				int n = 16 >> d;
				period = clampf(extPeriod * n, 0.002f, 120.f);
				clkPhase += dt / period;
				if (edge && (resetOnEdge || (edgeCount % n) == 0)) { tick = true; clkPhase = 0.f; }
				else if (lost && clkPhase >= 1.f) { clkPhase -= std::floor(clkPhase); tick = true; }
			} else if (d == 4) {                // x1
				period = extPeriod;
				clkPhase += dt / period;
				if (edge) { tick = true; clkPhase = 0.f; }
				else if (lost && clkPhase >= 1.f) { clkPhase -= std::floor(clkPhase); tick = true; }
			} else {                            // multiplications x2 x3 x4 x8
				static const int MULT[4] = {2, 3, 4, 8};
				int m = MULT[d - 5];
				period = clampf(extPeriod / m, 0.002f, 30.f);
				if (edge) { clkPhase = 0.f; multTick = 0; tick = true; }
				else {
					clkPhase += dt / period;
					if (clkPhase >= 1.f && (multTick < m - 1 || lost)) {
						clkPhase -= 1.f;
						if (multTick < 1000000) multTick++;
						tick = true;
					}
				}
			}
		} else {
			// "16 seconds at the bottom of the knob to 80Hz at the top"
			period = 16.f * std::pow(1.f / 1280.f, timeN);
			clkPhase += dt / period;
			if (clkPhase >= 1.f) { clkPhase -= std::floor(clkPhase); tick = true; }
			haveClk = false;
		}
		// "If the module has not received an external clock for at least four beats ... DIM WHITE"
		bool clockLost = extClock && (sinceClk > extPeriod * 4.f);

		// MUSICAL subdivision counts only, up into audio rate — arbitrary
		// counts like 37 stutter polyrhythmically against the clock and are
		// why it felt out of time
		int repeatsIdx = clamp((int) std::round(repeatsN * 19.f), 0, 19);
		int repeats = DB_RPT[repeatsIdx];

		if (tick && freezeTogglePending) {
			frozen = !frozen;
			freezeTogglePending = false;
			if (frozen && mixN < 0.02f) freezeMixWet = true;
			if (!frozen) freezeMixWet = false;
		}
		bool momentaryFreeze = (freezeMomentary && freezeButtonHigh && !shift)
			|| (gatesMomentary && freezeGateHigh);
		bool freezeActive = frozen || momentaryFreeze;
		if (momentaryFreeze && mixN < 0.02f) freezeMixWet = true;
		if (!freezeActive) freezeMixWet = false;
		if (freezeActive && !wasFreezeActive) freezeHead = writeHead;
		wasFreezeActive = freezeActive;

		if (tick) {
			// "Time ... is the rate at which a new audio buffer is ACQUIRED":
			// each tick acquires the division that just COMPLETED, and that is
			// what gets mangled during this division. Playing last division's
			// audio on the grid is why the hardware always sounds locked —
			// every subsection contains real, recent, beat-aligned music
			// (jumping into a still-unwritten forward section played audio
			// from a full buffer lap ago: the old "smashing" chaos).
			if (!freezeActive) {
				sectionLen = (samplesSinceTick > 32 && samplesSinceTick < bufLen)
					? samplesSinceTick                              // exact, sample-counted
					: clamp((int)(period * sr), 32, bufLen - 1);    // startup fallback
				sectionStart = writeHead - sectionLen;
				while (sectionStart < 0) sectionStart += bufLen;
			}
			else {
				// frozen: the section is the audio BEHIND the freeze point.
				// "Extending the time control down below where it was when the
				// signal was frozen will introduce artifacts of old data" —
				// a longer period reaches further back into buffer history.
				sectionLen = clamp((int)(period * sr), 32, bufLen - 1);
				sectionStart = freezeHead - sectionLen;
				while (sectionStart < 0) sectionStart += bufLen;
			}
			samplesSinceTick = 0;
			curSub[0] = curSub[1] = 0;
			readPos[0] = readPos[1] = sectionStart;
			tapeStop[0] = tapeStop[1] = 0.f;
			lastPhase[0] = lastPhase[1] = 0.f;
			subsActive[0] = subsActive[1] = -1;   // re-latch at the downbeat
			if (macro) rollMacro(bendN, breakN, repeats, repeatsIdx, bendEnabled, breakEnabled);
			clkBlink = 1.f;
		}
		samplesSinceTick++;

		// ---- write (background, always, unless frozen) ----
		float rawInL = inputs[LEFT_INPUT].getVoltage() * 0.2f;
		float rawInR = inputs[RIGHT_INPUT].isConnected() ? inputs[RIGHT_INPUT].getVoltage() * 0.2f : rawInL;
		float dcPole = std::exp(-2.f * (float) M_PI * 5.f / sr); // approximate the AC-coupled hardware input
		float inL = rawInL - dcPrevInL + dcPole * dcPrevOutL;
		float inR = rawInR - dcPrevInR + dcPole * dcPrevOutR;
		dcPrevInL = rawInL; dcPrevInR = rawInR; dcPrevOutL = inL; dcPrevOutR = inR;
		if (!freezeActive) {
			bufL[writeHead] = inL; bufR[writeHead] = inR;
			if (++writeHead >= bufLen) writeHead = 0;
		}

		// ---- independent stereo playback in Macro Unique mode ----
		float microOct = activeKnob[SK_BEND] * 6.f - 3.f;
		if (inputs[BEND_INPUT].isConnected()) microOct += inputs[BEND_INPUT].getVoltage();
		float microSpeed = std::pow(2.f, clampf(microOct, -3.f, 3.f));
		float wet[2] = {0.f, 0.f};
		float subPhase[2] = {0.f, 0.f};
		const std::vector<float>* channelBuf[2] = {&bufL, &bufR};
		for (int c = 0; c < 2; c++) {
			int target = std::max(1, macro && breakSubs[c] > 0 ? breakSubs[c] : repeats);
			target = clamp(target, 1, std::max(1, sectionLen / 4));
			// subdivision count latches at stutter boundaries: mid-slice knob
			// changes would re-phase the loop point off the grid
			if (subsActive[c] < 1) subsActive[c] = target;
			int subs = subsActive[c];
			float subLen = (float) sectionLen / subs;

			if (macro) {
				speedTarget[c] = macroSpeed[c];
				revNow[c] = macroRev[c];
				if (tapeStop[c] > 0.f) {
					// the stop completes within one clock division, keeping the
					// gesture beat-matched at any tempo
					tapeStop[c] = std::max(0.f, tapeStop[c] - dt / clampf(period, 0.05f, 4.f));
					speedTarget[c] *= tapeStop[c] * tapeStop[c];
				}
			} else {
				speedTarget[c] = microSpeed;
				revNow[c] = reverseEnabled;
				speedSlew[c] = 0.f;
			}
			if (speedSlew[c] > 1e-4f)
				speed[c] += (speedTarget[c] - speed[c]) * (1.f - std::exp(-dt / speedSlew[c]));
			else speed[c] = speedTarget[c];

			float silence = macro ? macroSilence[c] : (silenceEnabled ? breakN * 0.9f : 0.f);
			int want = curSub[c];
			if (!macro && !silenceEnabled)
				want = clamp((int)(breakN * subs), 0, subs - 1);   // Traverse target

			float subStart = sectionStart + curSub[c] * subLen;
			float rel = readPos[c] - subStart;
			rel -= std::floor(rel / subLen) * subLen;
			readPos[c] = subStart + rel;
			subPhase[c] = rel / subLen;
			// stutter boundary: latch pending subdivision/traverse changes HERE
			// so every change lands exactly on the grid
			bool wrapped = std::fabs(subPhase[c] - lastPhase[c]) > 0.5f;
			lastPhase[c] = subPhase[c];
			if (wrapped) {
				if (target != subs) subsActive[c] = target;
				if (want != curSub[c]) { curSub[c] = want; breakBlip = 0.3f; }
			}
			wet[c] = readBuf(*channelBuf[c], readPos[c]) + bendClick[c];
			bendClick[c] = 0.f;
			readPos[c] += speed[c] * (revNow[c] ? -1.f : 1.f);

			// Both Macro and Micro silence are synchronized duty cycles, up to 90%.
			if (silence > 0.f && subPhase[c] > (1.f - silence)) wet[c] = 0.f;
			if (windowing > 0.001f) {
				float w = clampf(subPhase[c] / windowing, 0.f, 1.f)
				        * clampf((1.f - subPhase[c]) / windowing, 0.f, 1.f);
				// normalize so the swell always reaches full volume ("only
				// reaching its full volume for a moment before fading back
				// out") — unnormalized, full windowing peaked at just 0.25
				if (windowing > 0.5f) {
					float pk = 0.5f / windowing;
					w /= pk * pk;
				}
				wet[c] *= w;
			}
		}
		float wetL = wet[0], wetR = wet[1];

		// stereo enhancement (Shift+Mix)
		if (stereoWidth > 0.001f) {
			float m = 0.5f * (wetL + wetR), s = 0.5f * (wetL - wetR) * (1.f + stereoWidth * 3.f);
			wetL = m + s; wetR = m - s;
		}

		applyCorrupt(corruptEffect, corruptN, wetL, wetR, sr);

		float mix = freezeMixWet ? 1.f : mixN;
		// "roughly in a range between input level and 14Vpp when using a lot of Corruption"
		outputs[LEFT_OUTPUT].setVoltage(clampf(lerpf(inL, wetL, mix) * 5.f, -7.f, 7.f));
		outputs[RIGHT_OUTPUT].setVoltage(clampf(lerpf(inR, wetR, mix) * 5.f, -7.f, 7.f));

		// ---- LEDs ----
		clkBlink = std::max(0.f, clkBlink - dt * 6.f);
		divBlip = std::max(0.f, divBlip - dt * 3.f);
		breakBlip = std::max(0.f, breakBlip - dt * 4.f);
		restoreBlip = std::max(0.f, restoreBlip - dt * 4.f);
		auto setLed = [&](int id, float value) {
			lights[id].setBrightnessSmooth(value * ledBrightness, dt);
		};

		// Clock: always lit in its mode colour (blue internal / white external,
		// like the hardware), pulsing brighter on each tick; dim steady white
		// when the external clock is lost; gold blip on div change.
		float cr = 0.f, cg = 0.f, cb = 0.f;
		float pulse = 0.3f + 0.7f * clkBlink;
		if (shift) { cg = gatesMomentary ? 1.f : 0.f; cb = gatesMomentary ? 0.f : 1.f; }
		else if (divBlip > 0.f) { cr = divBlip; cg = divBlip * 0.65f; cb = 0.f; }
		else if (!extClock) { cb = pulse; }
		else if (clockLost) { cr = cg = cb = 0.15f; }
		else { cr = cg = cb = pulse; }
		setLed(CLOCK_LIGHT_R, cr); setLed(CLOCK_LIGHT_G, cg); setLed(CLOCK_LIGHT_B, cb);

		// Mode: Macro blue / Micro green, or all-five blue / original-three green in Shift.
		setLed(MODE_LIGHT_R, 0.f);
		setLed(MODE_LIGHT_G, shift ? (originalCorruptOnly ? 1.f : 0.f) : (macro ? 0.f : 1.f));
		setLed(MODE_LIGHT_B, shift ? (originalCorruptOnly ? 0.f : 1.f) : (macro ? 1.f : 0.f));

		// Bend: Macro -> blue when enabled. Micro -> forward blue / on-octave cyan,
		// reverse green / on-octave-reverse gold.
		float br = 0.f, bg = 0.f, bb = 0.f;
		if (shift) { bg = stereoUnique ? 0.f : 1.f; bb = stereoUnique ? 1.f : 0.f; }
		else if (macro) bb = bendEnabled ? 1.f : 0.f;
		else {
			float oct = activeKnob[SK_BEND] * 6.f - 3.f;
			if (inputs[BEND_INPUT].isConnected()) oct += inputs[BEND_INPUT].getVoltage();
			bool onOct = std::abs(oct - std::round(oct)) < 0.03f;
			if (!reverseEnabled) { if (onOct) { bg = 1.f; bb = 1.f; } else bb = 1.f; }
			else           { if (onOct) { br = 1.f; bg = 0.65f; } else bg = 1.f; }
		}
		setLed(BEND_LIGHT_R, br); setLed(BEND_LIGHT_G, bg); setLed(BEND_LIGHT_B, bb);

		// Break: Macro -> blue when enabled. Micro -> off for Traverse, blue for Silence,
		// gold blip when the subsection changes.
		float kr = 0.f, kg = 0.f, kb = 0.f;
		if (shift && restoreBlip > 0.f) kb = 1.f;
		else if (shift) { kr = kg = kb = 0.5f + 0.5f * std::sin(args.frame * 0.0004f); }
		else if (macro) kb = breakEnabled ? 1.f : 0.f;
		else if (silenceEnabled) kb = 1.f;
		if (!shift && breakBlip > 0.f) { kr = std::max(kr, breakBlip); kg = std::max(kg, breakBlip * 0.65f); }
		setLed(BREAK_LIGHT_R, kr); setLed(BREAK_LIGHT_G, kg); setLed(BREAK_LIGHT_B, kb);

		// Corrupt: Decimate blue, Dropout green, Destroy gold, DJ Filter purple, Vinyl orange
		static const float CC[5][3] = {
			{0.f, 0.3f, 1.f}, {0.f, 1.f, 0.25f}, {1.f, 0.65f, 0.f}, {0.65f, 0.2f, 1.f}, {1.f, 0.35f, 0.f}
		};
		float on = (corruptN > 0.001f) ? 1.f : 0.25f;
		if (shift) {
			setLed(CORRUPT_LIGHT_R, 0.f);
			setLed(CORRUPT_LIGHT_G, corruptAsReset ? 1.f : 0.f);
			setLed(CORRUPT_LIGHT_B, corruptAsReset ? 0.f : 1.f);
		} else {
			setLed(CORRUPT_LIGHT_R, CC[corruptEffect][0] * on);
			setLed(CORRUPT_LIGHT_G, CC[corruptEffect][1] * on);
			setLed(CORRUPT_LIGHT_B, CC[corruptEffect][2] * on);
		}

		setLed(FREEZE_LIGHT_R, 0.f);
		setLed(FREEZE_LIGHT_G, shift ? (freezeMomentary ? 1.f : 0.f) : (freezeActive ? 0.35f : 0.f));
		setLed(FREEZE_LIGHT_B, shift ? (freezeMomentary ? 0.f : 1.f) : (freezeActive ? 1.f : 0.f));

		// Shift LED indicates the windowing amount: off = none, blue = default, then dim->bright white
		float sr_ = 0.f, sg = 0.f, sb = 0.f;
		if (shift) {
			if (windowing < 0.005f) { }
			else if (windowing < 0.05f) sb = 1.f;
			else { sr_ = sg = sb = 0.25f + windowing * 0.75f; }
		}
		setLed(SHIFT_LIGHT_R, sr_); setLed(SHIFT_LIGHT_G, sg); setLed(SHIFT_LIGHT_B, sb);
	}
};

struct BadSectorWidget : ModuleWidget {
	BadSectorWidget(BadSector* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/BadSector.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// big Time knob, then the shift / clock / mode row
		addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(16.5, 21.0)), module, BadSector::TIME_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<RedGreenBlueLight>>(mm2px(Vec(38.4, 23.0)), module, BadSector::SHIFT_PARAM, BadSector::SHIFT_LIGHT_R));
		addParam(createLightParamCentered<VCVLightBezel<RedGreenBlueLight>>(mm2px(Vec(50.0, 23.0)), module, BadSector::CLOCKBTN_PARAM, BadSector::CLOCK_LIGHT_R));
		addParam(createLightParamCentered<VCVLightBezel<RedGreenBlueLight>>(mm2px(Vec(61.6, 23.0)), module, BadSector::MODE_PARAM, BadSector::MODE_LIGHT_R));

		// bend / break / corrupt / freeze buttons
		addParam(createLightParamCentered<VCVLightBezel<RedGreenBlueLight>>(mm2px(Vec(29.0, 42.0)), module, BadSector::BENDBTN_PARAM, BadSector::BEND_LIGHT_R));
		addParam(createLightParamCentered<VCVLightBezel<RedGreenBlueLight>>(mm2px(Vec(40.0, 42.0)), module, BadSector::BREAKBTN_PARAM, BadSector::BREAK_LIGHT_R));
		addParam(createLightParamCentered<VCVLightBezel<RedGreenBlueLight>>(mm2px(Vec(51.0, 42.0)), module, BadSector::CORRUPTBTN_PARAM, BadSector::CORRUPT_LIGHT_R));
		addParam(createLightParamCentered<VCVLightBezel<RedGreenBlueLight>>(mm2px(Vec(62.0, 42.0)), module, BadSector::FREEZE_PARAM, BadSector::FREEZE_LIGHT_R));

		// repeats / mix down the left edge
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.0, 47.0)), module, BadSector::REPEATS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.0, 74.0)), module, BadSector::MIX_PARAM));

		// bend / break / corrupt knobs, each wired down to its CV jack
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(24.0, 66.0)), module, BadSector::BEND_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.0, 66.0)), module, BadSector::BREAK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(52.0, 66.0)), module, BadSector::CORRUPT_PARAM));

		// jacks: 5 columns x 3 rows
		float jx[5] = {8.5, 22.5, 36.0, 49.5, 62.5};
		float jy1 = 88.0, jy2 = 101.0, jy3 = 114.0;
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[0], jy1)), module, BadSector::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[1], jy1)), module, BadSector::BEND_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[2], jy1)), module, BadSector::BREAK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[3], jy1)), module, BadSector::CORRUPT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[4], jy1)), module, BadSector::FREEZE_INPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[0], jy2)), module, BadSector::LEFT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[1], jy2)), module, BadSector::BENDGATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[2], jy2)), module, BadSector::BREAKGATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[3], jy2)), module, BadSector::CORRUPTGATE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(jx[4], jy2)), module, BadSector::LEFT_OUTPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[0], jy3)), module, BadSector::RIGHT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[1], jy3)), module, BadSector::MIX_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[2], jy3)), module, BadSector::REPEATS_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jx[3], jy3)), module, BadSector::TIME_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(jx[4], jy3)), module, BadSector::RIGHT_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		BadSector* m = dynamic_cast<BadSector*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Corrupt effect",
			{"Decimate", "Dropout", "Destroy", "DJ Filter (extra)", "Vinyl Sim (extra)"}, &m->corruptSel));
		menu->addChild(createBoolPtrMenuItem("Original 3 corrupt effects only (hardware)", "", &m->originalCorruptOnly));
		menu->addChild(createBoolPtrMenuItem("Macro mode", "", &m->macro));
		menu->addChild(createBoolPtrMenuItem("External clock", "", &m->extClock));
		menu->addChild(createBoolPtrMenuItem("Stereo: unique per channel", "", &m->stereoUnique));
		menu->addChild(createMenuItem("Restore default settings", "", [m]() { m->restoreDefaults(); }));
	}
};

Model* modelBadSector = createModel<BadSector, BadSectorWidget>("BadSector");
