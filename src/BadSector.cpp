// Bad Sector — a stereo buffer-corruption and broken-playback processor.
//
// v2 panel architecture: six large controls (BUFFER, REPEAT, MIX, MICRO,
// DAMAGE, CV AMT). DAMAGE is one knob editing three independently stored
// values (Bend / Break / Corrupt) selected by an illuminated square button
// with soft takeover; CV AMT is the same pattern for three bipolar CV
// attenuverters. Micro and Clock modes, and all DSP, are unchanged from v1.
//
// Layout constants mirror gen_panel.py — keep them in sync.
#include "plugin.hpp"
#include "BsSelector.hpp"
#include <cmath>
#include <vector>

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// musical subdivision counts (powers of two + triplets) — everything that
// subdivides the clock picks from this table so stutters stay on the grid
static const int DB_RPT[20] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64,
                               96, 128, 192, 256, 384, 512, 768, 1024};

// mode colours: Bend cyan / Break amber / Corrupt red-orange
static const float SEL_COL[3][3] = {
	{0.15f, 0.85f, 1.f}, {1.f, 0.66f, 0.08f}, {1.f, 0.22f, 0.04f}
};

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
	enum ParamId {
		BUFFER_PARAM, REPEAT_PARAM, MIX_PARAM, DAMAGE_PARAM, CVAMT_PARAM, MICRO_PARAM,
		DMGSEL_PARAM, CVSEL_PARAM, MODE_PARAM, CLOCKBTN_PARAM, FREEZE_PARAM, PARAMS_LEN
	};
	enum InputId {
		IN_L_INPUT, IN_R_INPUT,
		BUFFER_CV_INPUT, REPEAT_CV_INPUT, MIX_CV_INPUT,
		BEND_CV_INPUT, BREAK_CV_INPUT, CORRUPT_CV_INPUT,
		BEND_GATE_INPUT, BREAK_GATE_INPUT, CORRUPT_GATE_INPUT, FREEZE_GATE_INPUT,
		CLOCK_INPUT, RESET_INPUT, INPUTS_LEN
	};
	enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		ENUMS(DMGSEL_LIGHT, 3), ENUMS(CVSEL_LIGHT, 3),
		ENUMS(MODE_LIGHT, 3), ENUMS(CLK_LIGHT, 3), ENUMS(FRZ_LIGHT, 3),
		DOT_DMG_LIGHT, DOT_CV_LIGHT, LIGHTS_LEN
	};

	// "over a minute of stereo audio"
	static constexpr float MAX_SECONDS = 64.f;
	std::vector<float> bufL, bufR;
	int bufLen = 0, writeHead = 0;

	// playback
	// double: at high buffer addresses float only resolves 0.25-sample
	// steps, which detunes fractional speeds and drifts loop points
	double readPos[2] = {0.0, 0.0};
	int sectionStart = 0, sectionLen = 4800;
	int curSub[2] = {0, 0};
	int samplesSinceTick = 0;
	int subsActive[2] = {-1, -1};
	float lastPhase[2] = {0.f, 0.f};
	float speed[2] = {1.f, 1.f};
	float speedTarget[2] = {1.f, 1.f};
	float speedSlew[2] = {0.f, 0.f};
	bool revNow[2] = {false, false};

	// the two shared three-channel editors
	BsSelector damage;   // bend / break / corrupt amounts (0..1)
	BsSelector cvAmt;    // bend / break / corrupt CV depth (0..1 -> -1..+1)

	// state
	int freezeHead = 0;
	bool wasFreezeActive = false;
	bool macro = true;
	bool frozen = false, bendOn = true, breakOn = true;
	bool microRev = false;
	bool microSilence = false;   // Traverse default
	int corruptSel = 0;
	float windowing = 0.02f;
	float stereoWidth = 0.f;
	bool stereoUnique = false;   // default/restore = Shared
	float ledBrightness = 1.f;
	bool gatesMomentary = false;
	bool freezeMomentary = false;
	bool originalCorruptOnly = true;
	bool microInMacro = false;   // MICRO knob as global varispeed under the Macro automation
	bool freezeMixWet = false;
	bool freezeTogglePending = false;
	bool freezeButtonWasHigh = false;
	bool resetDivisionPending = false;

	// clock
	bool extClock = false;
	float clkPhase = 0.f, extPeriod = 0.5f, sinceClk = 0.f;
	bool haveClk = false;
	int lastDiv = 4;
	int edgeCount = 0, multTick = 0;
	float divBlip = 0.f, clkBlink = 0.f;

	// macro per-clock decisions (per channel when stereoUnique)
	float macroSpeed[2] = {1.f, 1.f};
	bool macroRev[2] = {false, false};
	float macroSilence[2] = {0.f, 0.f};
	float tapeStop[2] = {0.f, 0.f};
	int breakSubs[2] = {0, 0};
	// tape soul: wow/flutter phases and vinyl pop envelopes (Bend)
	float wowPh[2] = {0.f, 0.5f};
	float flutPh[2] = {0.f, 0.3f};
	float popEnv[2] = {0.f, 0.f};

	// corrupt state
	float decHoldL = 0.f, decHoldR = 0.f; int decCount = 0;
	float dropEnv = 1.f; int dropTimer = 0;
	SVF djL, djR;
	float vinylPhase = 0.f;
	float vinylLpL = 0.f, vinylLpR = 0.f;
	float dcPrevInL = 0.f, dcPrevInR = 0.f, dcPrevOutL = 0.f, dcPrevOutR = 0.f;

	// telemetry for the reactive checksum artwork
	float uiBend = 0.f, uiBreak = 0.f, uiCorrupt = 0.f;
	float uiMicroOct = 0.f, uiTravBlip = 0.f;
	bool uiMicroRev = false;
	int uiDivIdx = -1;   // -1 = internal clock

	DbRng rng;
	dsp::BooleanTrigger dmgSelBtn, cvSelBtn, modeBtn, clockBtn;
	dsp::SchmittTrigger clockTrig, resetTrig, bendGate, breakGate, corruptGate, freezeGate;

	BadSector() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(BUFFER_PARAM, 0.f, 1.f, 0.5f, "Buffer (16s .. 80Hz, or clock div/mult)");
		configParam(REPEAT_PARAM, 0.f, 1.f, 0.f, "Repeat (musical subdivisions, up to audio rate)");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix (wet = the previous clock division)", "%", 0.f, 100.f);
		configParam(DAMAGE_PARAM, 0.f, 1.f, 0.f, "Damage (selected channel: Bend/Break/Corrupt)");
		configParam(CVAMT_PARAM, 0.f, 1.f, 0.75f, "CV amount (selected channel, bipolar)");
		configParam(MICRO_PARAM, 0.f, 1.f, 0.5f, "Micro playback speed (+/-3 oct)");
		configButton(DMGSEL_PARAM, "Damage channel (Bend/Break/Corrupt)");
		configButton(CVSEL_PARAM, "CV amount channel (Bend/Break/Corrupt)");
		configButton(MODE_PARAM, "Mode (Macro / Micro)");
		configButton(CLOCKBTN_PARAM, "Clock source (internal / external)");
		configButton(FREEZE_PARAM, "Freeze");
		configInput(IN_L_INPUT, "Left audio (normals to both channels)");
		configInput(IN_R_INPUT, "Right audio");
		configInput(BUFFER_CV_INPUT, "Buffer CV");
		configInput(REPEAT_CV_INPUT, "Repeat CV");
		configInput(MIX_CV_INPUT, "Mix CV");
		configInput(BEND_CV_INPUT, "Bend CV (1V/oct Micro pitch in Micro mode)");
		configInput(BREAK_CV_INPUT, "Break CV");
		configInput(CORRUPT_CV_INPUT, "Corrupt CV (<=0V disables Corrupt)");
		configInput(BEND_GATE_INPUT, "Bend gate (Macro on/off; Micro reverse)");
		configInput(BREAK_GATE_INPUT, "Break gate (Macro on/off; Micro traverse/silence)");
		configInput(CORRUPT_GATE_INPUT, "Corrupt gate (next corrupt effect)");
		configInput(FREEZE_GATE_INPUT, "Freeze gate");
		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset (resync clock / divisions)");
		configOutput(OUT_L_OUTPUT, "Left");
		configOutput(OUT_R_OUTPUT, "Right");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		damage.reset(0.f, 0.f, 0.f);
		cvAmt.reset(0.75f, 0.75f, 0.75f);
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
		windowing = 0.02f; bendOn = true; breakOn = true; frozen = false;
		macro = true; stereoUnique = false; corruptSel = 0;
		microRev = false; microSilence = false;
		gatesMomentary = false; freezeMomentary = false;
		freezeTogglePending = false; resetDivisionPending = false;
	}
	void onReset() override {
		std::fill(bufL.begin(), bufL.end(), 0.f); std::fill(bufR.begin(), bufR.end(), 0.f);
		writeHead = 0; readPos[0] = readPos[1] = 0.f; sectionStart = 0;
		curSub[0] = curSub[1] = 0;
		samplesSinceTick = 0; subsActive[0] = subsActive[1] = -1;
		lastPhase[0] = lastPhase[1] = 0.f;
		restoreDefaults(); extClock = false; stereoWidth = 0.f;
		freezeHead = 0; wasFreezeActive = false;
		wowPh[0] = flutPh[0] = 0.f; wowPh[1] = 0.5f; flutPh[1] = 0.3f;
		popEnv[0] = popEnv[1] = 0.f;
		ledBrightness = 1.f;
		originalCorruptOnly = true; freezeButtonWasHigh = false;
		damage.reset(0.f, 0.f, 0.f);
		cvAmt.reset(0.75f, 0.75f, 0.75f);
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
		json_object_set_new(r, "gatesMomentary", json_boolean(gatesMomentary));
		json_object_set_new(r, "freezeMomentary", json_boolean(freezeMomentary));
		json_object_set_new(r, "originalCorruptOnly", json_boolean(originalCorruptOnly));
		json_object_set_new(r, "microInMacro", json_boolean(microInMacro));
		json_t* dv = json_array();
		json_t* av = json_array();
		for (int i = 0; i < 3; i++) {
			json_array_append_new(dv, json_real(damage.vals[i]));
			json_array_append_new(av, json_real(cvAmt.vals[i]));
		}
		json_object_set_new(r, "damageVals", dv);
		json_object_set_new(r, "cvAmtVals", av);
		json_object_set_new(r, "damageSel", json_integer(damage.sel));
		json_object_set_new(r, "cvAmtSel", json_integer(cvAmt.sel));
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
		if (json_t* j = json_object_get(r, "gatesMomentary")) gatesMomentary = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "freezeMomentary")) freezeMomentary = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "originalCorruptOnly")) originalCorruptOnly = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "microInMacro")) microInMacro = json_boolean_value(j);
		json_t* dv = json_object_get(r, "damageVals");
		json_t* av = json_object_get(r, "cvAmtVals");
		for (int i = 0; i < 3; i++) {
			if (dv && json_array_size(dv) == 3) damage.vals[i] = (float) json_real_value(json_array_get(dv, i));
			if (av && json_array_size(av) == 3) cvAmt.vals[i] = (float) json_real_value(json_array_get(av, i));
		}
		if (json_t* j = json_object_get(r, "damageSel")) damage.sel = clamp((int) json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(r, "cvAmtSel")) cvAmt.sel = clamp((int) json_integer_value(j), 0, 2);
		if (originalCorruptOnly && corruptSel >= 3) corruptSel = 0;
		// snap the knobs to the restored channels' stored values
		damage.caught = true;
		damage.lastKnob = damage.vals[damage.sel];
		params[DAMAGE_PARAM].setValue(damage.vals[damage.sel]);
		cvAmt.caught = true;
		cvAmt.lastKnob = cvAmt.vals[cvAmt.sel];
		params[CVAMT_PARAM].setValue(cvAmt.vals[cvAmt.sel]);
	}

	float readBuf(const std::vector<float>& b, double pos) {
		pos -= std::floor(pos / bufLen) * bufLen;
		int i0 = (int) pos; float fr = (float)(pos - i0);
		int i1 = i0 + 1; if (i1 >= bufLen) i1 = 0;
		return lerpf(b[i0], b[i1], fr);
	}

	void applyCorrupt(int effect, float amt, float& l, float& r, float sr) {
		if (amt <= 0.001f) return;
		switch (effect) {
			case 0: {  // Decimate — variable bit-crushing and downsampling
				int hold = 1 + (int)(amt * amt * 54.f);
				float bits = 16.f - amt * 14.f;
				if (--decCount <= 0) { decCount = hold; decHoldL = l; decHoldR = r; }
				float q = std::pow(2.f, bits - 1.f);
				l = std::round(decHoldL * q) / q;
				r = std::round(decHoldR * q) / q;
			} break;
			case 1: {  // Dropout — left: fewer but longer; right: more but shorter
				if (--dropTimer <= 0) {
					float gapLen  = lerpf(0.35f, 0.02f, amt) * sr;
					float betweenLen = lerpf(1.2f, 0.05f, amt) * sr;
					if (dropEnv < 0.5f) { dropEnv = 1.f; dropTimer = (int)(betweenLen * (0.5f + rng.f())); }
					else                { dropEnv = 0.f; dropTimer = (int)(gapLen * (0.5f + rng.f())); }
				}
				l *= dropEnv; r *= dropEnv;
			} break;
			case 2: {  // Destroy — soft saturation then absolute devastation
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
			case 3: {  // DJ Filter (extra) — LP below noon, HP above
				float lp, hp, dummy;
				if (amt < 0.48f) {
					float t = amt / 0.48f;
					float fc = 30.f * std::pow(666.f, t);
					float g = std::tan(M_PI * clampf(fc, 20.f, 18000.f) / sr);
					djL.process(l, g, 0.8f, lp, dummy); l = lp;
					djR.process(r, g, 0.8f, lp, dummy); r = lp;
				} else if (amt > 0.52f) {
					float t = (amt - 0.52f) / 0.48f;
					float fc = 20.f * std::pow(600.f, t);
					float g = std::tan(M_PI * clampf(fc, 20.f, 18000.f) / sr);
					djL.process(l, g, 0.8f, dummy, hp); l = hp;
					djR.process(r, g, 0.8f, dummy, hp); r = hp;
				}
			} break;
			default: {  // Vinyl Sim (extra) — dust, pops and colouring
				if (rng.f() < amt * 0.0008f) {
					float c = rng.bip() * amt * 0.8f;
					l += c; r += c * 0.7f;
				}
				if (rng.f() < amt * 0.02f) {
					float c = rng.bip() * amt * 0.15f;
					l += c; r += c;
				}
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

	// Every clock division, Macro mode rolls new manipulations. Both Bend and
	// Break use CUMULATIVE knob zones.
	void rollMacro(float bendAmt, float breakAmt, int repeats, int repeatsIdx, bool bendEnabled, bool breakEnabled) {
		int nCh = stereoUnique ? 2 : 1;
		if (bendEnabled && bendAmt > 0.001f) {
			int z = (int) std::ceil(bendAmt * 6.f);
			float top = clampf(bendAmt * 6.f - (z - 1), 0.f, 1.f);
			auto za = [&](int k) { return (k < z) ? 1.f : (k == z ? top : 0.f); };
			// pitch changes come in octaves and fifths, so it stays musical
			static const float R2[4] = {2.f, 0.5f, 1.5f, 0.75f};
			static const float R3[4] = {4.f, 0.25f, 3.f, 1.f / 3.f};
			for (int c = 0; c < nCh; c++) {
				float sp = 1.f; bool rv = false;
				if (z >= 1 && rng.f() < za(1) * 0.5f) rv = true;
				if (z >= 2 && rng.f() < za(2) * 0.5f) sp *= R2[rng.u32() & 3];
				if (z >= 3 && rng.f() < za(3) * 0.4f) sp *= R3[rng.u32() & 3];
				if (z >= 4 && rng.f() < za(4) * 0.3f) tapeStop[c] = 1.f;
				macroSpeed[c] = sp; macroRev[c] = rv;
				speedSlew[c] = (z >= 5) ? za(5) : 0.f;   // 0..1, scaled by period at use
			}
			if (!stereoUnique) {
				macroSpeed[1] = macroSpeed[0]; macroRev[1] = macroRev[0];
				tapeStop[1] = tapeStop[0]; speedSlew[1] = speedSlew[0];
			}
		} else {
			macroSpeed[0] = macroSpeed[1] = 1.f;
			macroRev[0] = macroRev[1] = false;
			speedSlew[0] = speedSlew[1] = 0.f;
		}

		if (breakEnabled && breakAmt > 0.001f) {
			int z = (int) std::ceil(breakAmt * 6.f);
			float top = clampf(breakAmt * 6.f - (z - 1), 0.f, 1.f);
			auto za = [&](int k) { return (k < z) ? 1.f : (k == z ? top : 0.f); };
			for (int c = 0; c < nCh; c++) {
				// extra repeats always come from the musical table
				int subs = std::max(1, repeats);
				if (z >= 1 && rng.f() < za(1) * 0.5f) subs = std::max(subs, 2);
				if (z >= 3 && rng.f() < za(3) * 0.6f)
					subs = std::max(subs, DB_RPT[std::min(19, repeatsIdx + 1 + (int)(rng.f() * 4.f))]);
				if (z >= 4 && rng.f() < za(4) * 0.5f)
					subs = std::max(subs, DB_RPT[9 + (int)(rng.f() * 5.f)]);
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

		// ---- shared editors: selector buttons recall the stored value (the
		// knob pointer snaps to show it), then the knob edits directly ----
		if (dmgSelBtn.process(params[DMGSEL_PARAM].getValue() > 0.5f)) {
			damage.vals[damage.sel] = params[DAMAGE_PARAM].getValue();
			damage.sel = (damage.sel + 1) % 3;
			damage.caught = true;
			damage.lastKnob = damage.vals[damage.sel];
			params[DAMAGE_PARAM].setValue(damage.vals[damage.sel]);
		}
		if (cvSelBtn.process(params[CVSEL_PARAM].getValue() > 0.5f)) {
			cvAmt.vals[cvAmt.sel] = params[CVAMT_PARAM].getValue();
			cvAmt.sel = (cvAmt.sel + 1) % 3;
			cvAmt.caught = true;
			cvAmt.lastKnob = cvAmt.vals[cvAmt.sel];
			params[CVAMT_PARAM].setValue(cvAmt.vals[cvAmt.sel]);
		}
		damage.track(params[DAMAGE_PARAM].getValue());
		cvAmt.track(params[CVAMT_PARAM].getValue());

		// bipolar CV depths: knob 0..1 -> -1..+1, centre = zero modulation
		float att[3];
		for (int i = 0; i < 3; i++) att[i] = cvAmt.vals[i] * 2.f - 1.f;

		// ---- effective control values ----
		float timeN = clampf(params[BUFFER_PARAM].getValue() + inputs[BUFFER_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float repeatsN = clampf(params[REPEAT_PARAM].getValue() + inputs[REPEAT_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float mixN = clampf(params[MIX_PARAM].getValue() + inputs[MIX_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		float bendN = clampf(damage.vals[0] + inputs[BEND_CV_INPUT].getVoltage() * 0.1f * att[0], 0.f, 1.f);
		float breakN = clampf(damage.vals[1] + inputs[BREAK_CV_INPUT].getVoltage() * 0.1f * att[1], 0.f, 1.f);
		float corruptN = damage.vals[2];
		if (inputs[CORRUPT_CV_INPUT].isConnected()) {
			float cv = inputs[CORRUPT_CV_INPUT].getVoltage();
			corruptN = (cv <= 0.f) ? 0.f : clampf(damage.vals[2] + cv * 0.1f * att[2], 0.f, 1.f);
		}

		// ---- buttons + gates ----
		if (modeBtn.process(params[MODE_PARAM].getValue() > 0.5f)) macro = !macro;
		if (clockBtn.process(params[CLOCKBTN_PARAM].getValue() > 0.5f)) extClock = !extClock;
		bool freezeButtonHigh = params[FREEZE_PARAM].getValue() > 0.5f;
		bool freezePress = !freezeButtonWasHigh && freezeButtonHigh;
		bool freezeRelease = freezeButtonWasHigh && !freezeButtonHigh;
		freezeButtonWasHigh = freezeButtonHigh;
		if (!freezeMomentary && freezeRelease)
			freezeTogglePending = !freezeTogglePending;
		(void) freezePress;

		bool bendGateHigh = inputs[BEND_GATE_INPUT].getVoltage() >= 0.4f;
		bool breakGateHigh = inputs[BREAK_GATE_INPUT].getVoltage() >= 0.4f;
		bool corruptGateHigh = inputs[CORRUPT_GATE_INPUT].getVoltage() >= 0.4f;
		bool freezeGateHigh = inputs[FREEZE_GATE_INPUT].getVoltage() >= 0.4f;
		bool bendGateEdge = bendGate.process(inputs[BEND_GATE_INPUT].getVoltage(), 0.1f, 0.4f);
		bool breakGateEdge = breakGate.process(inputs[BREAK_GATE_INPUT].getVoltage(), 0.1f, 0.4f);
		bool corruptGateEdge = corruptGate.process(inputs[CORRUPT_GATE_INPUT].getVoltage(), 0.1f, 0.4f);
		bool freezeGateEdge = freezeGate.process(inputs[FREEZE_GATE_INPUT].getVoltage(), 0.1f, 0.4f);

		if (!gatesMomentary) {
			if (bendGateEdge) { if (macro) bendOn = !bendOn; else microRev = !microRev; }
			if (breakGateEdge) { if (macro) breakOn = !breakOn; else microSilence = !microSilence; }
			if (freezeGateEdge) freezeTogglePending = !freezeTogglePending;
			if (corruptGateEdge)
				corruptSel = (corruptSel + 1) % (originalCorruptOnly ? 3 : 5);
		}
		bool bendEnabled = bendOn || (gatesMomentary && bendGateHigh);
		bool breakEnabled = breakOn || (gatesMomentary && breakGateHigh);
		bool reverseEnabled = microRev || (gatesMomentary && bendGateHigh);
		bool silenceEnabled = microSilence || (gatesMomentary && breakGateHigh);
		int corruptEffect = corruptSel;
		if (gatesMomentary && corruptGateHigh)
			corruptEffect = (corruptEffect + 1) % (originalCorruptOnly ? 3 : 5);

		// ---- clock ----
		bool tick = false;
		float period;
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 0.4f)) {
			if (extClock) resetDivisionPending = true;
			else { clkPhase = 0.f; tick = true; }
		}
		if (extClock) {
			int d = clamp((int)(timeN * 8.99f), 0, 8);
			if (d != lastDiv) { lastDiv = d; divBlip = 0.35f; }
			uiDivIdx = d;
			sinceClk += dt;
			bool edge = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 0.4f);
			bool resetOnEdge = edge && resetDivisionPending;
			if (edge) {
				if (haveClk) extPeriod = clampf(sinceClk, 0.001f, 30.f);
				haveClk = true; sinceClk = 0.f;
				if (resetOnEdge) { edgeCount = 0; resetDivisionPending = false; }
				else edgeCount++;
			}
			// hard-lock to edges while the clock is present; free-run on the
			// measured period once edges stop so audio never dies
			bool lost = sinceClk > extPeriod * 1.1f;
			if (d <= 3) {                       // divisions /16 /8 /4 /2
				int n = 16 >> d;
				period = clampf(extPeriod * n, 0.001f, 120.f);
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
				period = clampf(extPeriod / m, 0.001f, 30.f);
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
			period = 16.f * std::pow(1.f / 1280.f, timeN);
			clkPhase += dt / period;
			if (clkPhase >= 1.f) { clkPhase -= std::floor(clkPhase); tick = true; }
			haveClk = false;
			uiDivIdx = -1;
		}
		bool clockLost = extClock && (sinceClk > extPeriod * 4.f);

		// musical subdivision counts only, up into audio rate. The curve keeps
		// rhythmic counts (1..16) in the first half of the travel and saves
		// audio-rate for the top stretch — linear indexing hit buzz at noon.
		int repeatsIdx = clamp((int) std::round(std::pow(repeatsN, 1.7f) * 19.f), 0, 19);
		int repeats = DB_RPT[repeatsIdx];

		if (tick && freezeTogglePending) {
			frozen = !frozen;
			freezeTogglePending = false;
			if (frozen && mixN < 0.02f) freezeMixWet = true;
			if (!frozen) freezeMixWet = false;
		}
		bool momentaryFreeze = (freezeMomentary && freezeButtonHigh)
			|| (gatesMomentary && freezeGateHigh);
		bool freezeActive = frozen || momentaryFreeze;
		if (momentaryFreeze && mixN < 0.02f) freezeMixWet = true;
		if (!freezeActive) freezeMixWet = false;
		if (freezeActive && !wasFreezeActive) freezeHead = writeHead;
		wasFreezeActive = freezeActive;

		if (tick) {
			// each tick acquires the just-completed division; that is what
			// gets mangled during this division (always beat-aligned audio)
			if (!freezeActive) {
				sectionLen = (samplesSinceTick > 32 && samplesSinceTick < bufLen)
					? samplesSinceTick
					: clamp((int)(period * sr), 32, bufLen - 1);
				sectionStart = writeHead - sectionLen;
				while (sectionStart < 0) sectionStart += bufLen;
			}
			else {
				// frozen: the window reaches BACK from the freeze point, so
				// lengthening Buffer digs into older audio history
				sectionLen = clamp((int)(period * sr), 32, bufLen - 1);
				sectionStart = freezeHead - sectionLen;
				while (sectionStart < 0) sectionStart += bufLen;
			}
			samplesSinceTick = 0;
			curSub[0] = curSub[1] = 0;
			readPos[0] = readPos[1] = sectionStart;
			tapeStop[0] = tapeStop[1] = 0.f;
			lastPhase[0] = lastPhase[1] = 0.f;
			subsActive[0] = subsActive[1] = -1;
			if (macro) rollMacro(bendN, breakN, repeats, repeatsIdx, bendEnabled, breakEnabled);
			clkBlink = 1.f;
		}
		samplesSinceTick++;

		// ---- write (background, always, unless frozen) ----
		float rawInL = inputs[IN_L_INPUT].getVoltage() * 0.2f;
		float rawInR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() * 0.2f : rawInL;
		float dcPole = std::exp(-2.f * (float) M_PI * 5.f / sr);
		float inL = rawInL - dcPrevInL + dcPole * dcPrevOutL;
		float inR = rawInR - dcPrevInR + dcPole * dcPrevOutR;
		dcPrevInL = rawInL; dcPrevInR = rawInR; dcPrevOutL = inL; dcPrevOutR = inR;
		if (!freezeActive) {
			bufL[writeHead] = inL; bufR[writeHead] = inR;
			if (++writeHead >= bufLen) writeHead = 0;
		}

		// ---- playback ----
		float microOct = params[MICRO_PARAM].getValue() * 6.f - 3.f;
		if (inputs[BEND_CV_INPUT].isConnected() && !macro)
			microOct += inputs[BEND_CV_INPUT].getVoltage();   // 1V/oct in Micro
		float microSpeed = std::pow(2.f, clampf(microOct, -3.f, 3.f));
		// tape wow & flutter phases (Bend character; macro only, so Micro
		// stays exact for 1V/oct melodies)
		wowPh[0] += dt * 0.55f;  wowPh[1] += dt * 0.62f;
		flutPh[0] += dt * 7.3f;  flutPh[1] += dt * 6.8f;
		for (int c = 0; c < 2; c++) {
			if (wowPh[c] >= 1.f) wowPh[c] -= 1.f;
			if (flutPh[c] >= 1.f) flutPh[c] -= 1.f;
		}
		float popDecay = std::exp(-1.f / (0.0018f * sr));
		float wet[2] = {0.f, 0.f};
		float subPhase[2] = {0.f, 0.f};
		const std::vector<float>* channelBuf[2] = {&bufL, &bufR};
		for (int c = 0; c < 2; c++) {
			int target = std::max(1, macro && breakSubs[c] > 0 ? breakSubs[c] : repeats);
			target = clamp(target, 1, std::max(1, sectionLen / 4));
			if (subsActive[c] < 1) subsActive[c] = target;
			int subs = subsActive[c];
			double subLen = (double) sectionLen / subs;

			if (macro) {
				// optional departure: MICRO knob transposes the whole mangling
				speedTarget[c] = macroSpeed[c] * (microInMacro ? microSpeed : 1.f);
				revNow[c] = macroRev[c];
				if (tapeStop[c] > 0.f) {
					tapeStop[c] = std::max(0.f, tapeStop[c] - dt / clampf(period, 0.05f, 4.f));
					speedTarget[c] *= tapeStop[c] * tapeStop[c];
				}
			} else {
				speedTarget[c] = microSpeed;
				revNow[c] = reverseEnabled;
				speedSlew[c] = 0.f;
			}
			// slew scaled to the division so glides complete musically at any
			// tempo; the 4ms floor de-zippers hard varispeed switches
			float slewSec = std::max(0.004f, speedSlew[c] * 0.35f * clampf(period, 0.05f, 4.f));
			speed[c] += (speedTarget[c] - speed[c]) * (1.f - std::exp(-dt / slewSec));

			float silence = macro ? macroSilence[c] : (silenceEnabled ? breakN * 0.9f : 0.f);
			int want = curSub[c];
			if (!macro && !silenceEnabled)
				want = clamp((int)(breakN * subs), 0, subs - 1);   // Traverse

			double subStart = sectionStart + curSub[c] * subLen;
			double rel = readPos[c] - subStart;
			rel -= std::floor(rel / subLen) * subLen;
			readPos[c] = subStart + rel;
			subPhase[c] = (float)(rel / subLen);
			// stutter boundary: latch pending subdivision/traverse changes so
			// every change lands exactly on the grid
			bool wrapped = std::fabs(subPhase[c] - lastPhase[c]) > 0.5f;
			lastPhase[c] = subPhase[c];
			if (wrapped) {
				if (target != subs) subsActive[c] = target;
				if (want != curSub[c]) {
					curSub[c] = want;
					if (!macro) uiTravBlip = 1.f;   // hardware: gold blip on traverse
				}
			}
			wet[c] = readBuf(*channelBuf[c], readPos[c]);
			// Bend tape character: wow/flutter wobble + scattered vinyl pops
			float spd = speed[c];
			if (macro && bendEnabled && bendN > 0.001f) {
				int pc = stereoUnique ? c : 0;
				float wf = bendN * (0.007f * std::sin(2.f * (float) M_PI * wowPh[pc])
				                  + 0.0025f * std::sin(2.f * (float) M_PI * flutPh[pc]));
				spd *= 1.f + wf;
				if (rng.f() < bendN * bendN * 22.f * dt)
					popEnv[c] = (0.03f + rng.f() * 0.15f) * bendN * (rng.f() < 0.5f ? -1.f : 1.f);
			}
			popEnv[c] *= popDecay;
			wet[c] += popEnv[c] * (0.6f + 0.4f * rng.bip());
			readPos[c] += (double) spd * (revNow[c] ? -1.0 : 1.0);

			if (silence > 0.f && subPhase[c] > (1.f - silence)) wet[c] = 0.f;
			if (windowing > 0.001f) {
				float w = clampf(subPhase[c] / windowing, 0.f, 1.f)
				        * clampf((1.f - subPhase[c]) / windowing, 0.f, 1.f);
				// peak-normalize so full windowing still reaches full volume
				if (windowing > 0.5f) {
					float pk = 0.5f / windowing;
					w /= pk * pk;
				}
				wet[c] *= w;
			}
		}
		float wetL = wet[0], wetR = wet[1];

		if (stereoWidth > 0.001f) {
			float m = 0.5f * (wetL + wetR), s = 0.5f * (wetL - wetR) * (1.f + stereoWidth * 3.f);
			wetL = m + s; wetR = m - s;
		}

		applyCorrupt(corruptEffect, corruptN, wetL, wetR, sr);

		// telemetry for the reactive artwork
		uiMicroOct = microOct;
		uiMicroRev = reverseEnabled;
		uiTravBlip = std::max(0.f, uiTravBlip - dt * 4.f);
		uiBend = macro ? (bendEnabled ? bendN : 0.f) : std::fabs(microOct) / 3.f;
		uiBreak = macro ? (breakEnabled ? breakN : 0.f) : breakN;
		uiCorrupt = corruptN;

		// equal-power crossfade: a linear blend makes the 50% point read loud
		// (correlated doubling) and full wet feel like a volume drop
		float mix = freezeMixWet ? 1.f : mixN;
		float dryG = std::cos(mix * (float) M_PI * 0.5f);
		float wetG = std::sin(mix * (float) M_PI * 0.5f);
		outputs[OUT_L_OUTPUT].setVoltage(clampf((inL * dryG + wetL * wetG) * 5.f, -7.f, 7.f));
		outputs[OUT_R_OUTPUT].setVoltage(clampf((inR * dryG + wetR * wetG) * 5.f, -7.f, 7.f));

		// ---- LEDs ----
		clkBlink = std::max(0.f, clkBlink - dt * 6.f);
		divBlip = std::max(0.f, divBlip - dt * 3.f);
		auto setLed = [&](int id, float value) {
			lights[id].setBrightnessSmooth(value * ledBrightness, dt);
		};

		// selector buttons: mode colour; blink while awaiting soft pickup
		float dBlink = damage.caught ? 1.f : (0.35f + 0.65f * (std::sin(args.frame * 0.0006f) > 0.f ? 1.f : 0.f));
		float aBlink = cvAmt.caught ? 1.f : (0.35f + 0.65f * (std::sin(args.frame * 0.0006f) > 0.f ? 1.f : 0.f));
		// the Bend channel is inert in Micro (manual speed replaces the
		// automation) — dim its colour there so the knob never reads as dead
		float dAct = (!macro && damage.sel == 0) ? 0.25f : 1.f;
		float aAct = (!macro && cvAmt.sel == 0) ? 0.25f : 1.f;
		for (int i = 0; i < 3; i++) {
			setLed(DMGSEL_LIGHT + i, SEL_COL[damage.sel][i] * dBlink * dAct);
			setLed(CVSEL_LIGHT + i, SEL_COL[cvAmt.sel][i] * aBlink * aAct);
		}
		// dots: the selected channel's stored value at a glance
		setLed(DOT_DMG_LIGHT, 0.1f + 0.9f * damage.vals[damage.sel]);
		setLed(DOT_CV_LIGHT, 0.1f + 0.9f * std::fabs(att[cvAmt.sel]));

		setLed(MODE_LIGHT + 0, 0.f);
		setLed(MODE_LIGHT + 1, macro ? 0.f : 1.f);
		setLed(MODE_LIGHT + 2, macro ? 1.f : 0.f);

		float cr = 0.f, cg = 0.f, cb = 0.f;
		float pulse = 0.3f + 0.7f * clkBlink;
		if (divBlip > 0.f) { cr = divBlip; cg = divBlip * 0.65f; }
		else if (!extClock) { cb = pulse; }
		else if (clockLost) { cr = cg = cb = 0.15f; }
		else { cr = cg = cb = pulse; }
		setLed(CLK_LIGHT + 0, cr); setLed(CLK_LIGHT + 1, cg); setLed(CLK_LIGHT + 2, cb);

		setLed(FRZ_LIGHT + 0, 0.f);
		setLed(FRZ_LIGHT + 1, freezeActive ? 0.8f : 0.f);
		setLed(FRZ_LIGHT + 2, freezeActive ? 1.f : (freezeTogglePending ? 0.25f : 0.f));
	}
};

// ------------------------------------------------------ custom hardware ----
struct BsScrew : app::SvgScrew {
	BsScrew() { setSvg(Svg::load(asset::plugin(pluginInstance, "res/components/screw.svg"))); }
};
struct BsPort : app::SvgPort {
	BsPort() { setSvg(Svg::load(asset::plugin(pluginInstance, "res/components/port.svg"))); }
};
struct BsSqButton : app::SvgSwitch {
	BsSqButton() {
		momentary = true;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/sqbtn_0.svg")));
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/sqbtn_1.svg")));
	}
};
struct BsSqButtonSmall : app::SvgSwitch {
	BsSqButtonSmall() {
		momentary = true;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/sqbtn_s0.svg")));
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/sqbtn_s1.svg")));
	}
};
// square light insert for the square buttons
struct BsSqLight : RedGreenBlueLight {
	BsSqLight() { box.size = mm2px(math::Vec(4.6f, 4.6f)); }
	void drawBackground(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, mm2px(0.45f));
		nvgFillColor(args.vg, bgColor);
		nvgFill(args.vg);
	}
	void drawLight(const DrawArgs& args) override {
		if (color.a <= 0.f) return;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, mm2px(0.45f));
		nvgFillColor(args.vg, color);
		nvgFill(args.vg);
	}
	// the default size-scaled halo floods the panel from these large squares
	void drawHalo(const DrawArgs& args) override {}
};
struct BsSqLightSmall : BsSqLight {
	BsSqLightSmall() { box.size = mm2px(math::Vec(3.4f, 3.4f)); }
};
struct CyanLight : GrayModuleLightWidget {
	CyanLight() { addBaseColor(nvgRGB(0x35, 0xd3, 0xe0)); }
};

// ---------------------------------------------- reactive checksum artwork ----
// Low DAMAGE: mostly intact data lines. Bend: horizontal displacement.
// Break: missing/repeated sections. Corrupt: noise blocks + unstable
// checksum characters. Subtle by design — labels stay readable.
struct BsChecksumArt : TransparentWidget {
	BadSector* module = nullptr;
	// zone in mm — between the knob columns, above the mode buttons
	static constexpr float X0 = 28.5f, X1 = 52.8f, Y0 = 16.5f, Y1 = 51.0f;
	float phase = 0.f;

	static uint32_t hash(int a, int b, int t) {
		uint32_t h = (uint32_t)(a * 73856093) ^ (uint32_t)(b * 19349663) ^ (uint32_t)(t * 83492791);
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		return h;
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float dt = clampf(APP->window->getLastFrameDuration(), 0.f, 0.05f);
		if (dt <= 0.f) dt = 1.f / 60.f;

		float bend = 0.15f, brk = 0.1f, corrupt = 0.05f, blink = 0.f;
		bool frozen = false;
		int divIdx = -1, corruptSel = 0;
		if (module) {
			bend = module->uiBend; brk = module->uiBreak; corrupt = module->uiCorrupt;
			blink = module->clkBlink; frozen = module->wasFreezeActive;
			divIdx = module->uiDivIdx; corruptSel = module->corruptSel;
		}
		if (!frozen) phase += dt;
		int slowT = (int)(phase * 3.f);
		float total = clampf(bend * 0.5f + brk * 0.35f + corrupt * 0.45f, 0.f, 1.f);

		NVGcolor ink = nvgRGBA(0xec, 0xe8, 0xdd, (unsigned char)(0x52 + blink * 0x30));
		// each damage channel paints in its selector colour:
		// Bend cyan / Break amber / Corrupt red-orange
		NVGcolor bendC = nvgRGBA(0x26, 0xd9, 0xff, 0xb4);
		NVGcolor breakC = nvgRGBA(0xff, 0xa8, 0x15, 0xb4);
		NVGcolor corruptC = nvgRGBA(0xff, 0x38, 0x0a, 0xbc);

		float rowH = 1.55f;
		int rows = (int)((Y1 - Y0 - 4.f) / rowH);
		for (int r = 0; r < rows; r++) {
			float y = Y0 + 4.f + r * rowH;
			uint32_t h = hash(r, 0, slowT);
			// Break: missing rows, occasionally a repeated (shifted) row
			bool rowRepeated = false;
			if (brk > 0.01f && (h & 0xFF) < brk * 120.f) {
				if (((h >> 20) & 3) == 0) { y = Y0 + 4.f + ((r + 1) % rows) * rowH; rowRepeated = true; }
				else continue;                                                     // missing
			}
			// Bend: horizontal displacement, wavier as bend rises
			float dx = std::sin(y * 0.35f + phase * 0.8f) * bend * 5.f
			         + (((h >> 8) & 0xFF) / 255.f - 0.5f) * total * 6.f;
			// data line: a few segments per row
			int segs = 1 + ((h >> 16) & 3);
			float x = X0 + 1.f + ((h >> 10) & 7) * 0.7f + dx;
			for (int sgi = 0; sgi < segs && x < X1 - 2.f; sgi++) {
				uint32_t sh = hash(r, sgi + 1, slowT);
				float w = 1.2f + (sh & 0xF) * (0.32f + total * 0.25f);
				w = std::min(w, X1 - 1.f - x);
				if (w <= 0.f) break;
				bool bright = (sh & 0x300) == 0;
				nvgBeginPath(vg);
				nvgRect(vg, mm2px(x), mm2px(y), mm2px(w), mm2px(0.62f + (bright ? 0.2f : 0.f)));
				// channel colours: corrupt red-orange noise blocks, break amber
				// on repeated/broken rows, bend cyan on displaced fragments
				NVGcolor col = ink;
				if (corrupt > 0.01f && ((sh >> 12) & 0xFF) < corrupt * 90.f)
					col = corruptC;
				else if (rowRepeated || (brk > 0.01f && ((sh >> 18) & 0xFF) < brk * 45.f))
					col = breakC;
				else if (bend > 0.01f && ((sh >> 14) & 0xFF) < bend * 60.f)
					col = bendC;
				else if (bright) col = nvgRGBA(0xec, 0xe8, 0xdd, 0x92);
				nvgFillColor(vg, col);
				nvgFill(vg);
				x += w + 0.8f + ((sh >> 4) & 7) * 0.55f * (1.f + total);
			}
		}

		// status line: clock division + corrupt effect as a live "checksum"
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font) {
			static const char* DIVS[9] = {"/16", "/8", "/4", "/2", "x1", "x2", "x3", "x4", "x8"};
			static const char* FX[5] = {"DEC", "DRP", "DST", "DJF", "VYL"};
			char txt[48];
			uint32_t ck = hash(7, 9, slowT);
			if (corrupt > 0.4f) ck ^= hash(3, 1, (int)(phase * 17.f));   // unstable checksum
			snprintf(txt, sizeof(txt), "%s %s %04X", divIdx < 0 ? "INT" : DIVS[divIdx],
			         FX[corruptSel], (unsigned)(ck & 0xFFFF));
			nvgFontFaceId(vg, font->handle);
			nvgFontSize(vg, mm2px(1.9f));
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
			// hardware Break LED behaviour: gold blip when the traverse
			// subsection changes
			float tb = module ? module->uiTravBlip : 0.f;
			NVGcolor stat = (tb > 0.f)
				? nvgRGBA(0xff, (unsigned char)(0xc2 - 0x36 * (1.f - tb)), 0x3e, 0xd8)
				: nvgRGBA(0x8f, 0x8c, 0x83, 0xc8);
			nvgFillColor(vg, stat);
			nvgText(vg, mm2px((X0 + X1) * 0.5f), mm2px(Y0), txt, NULL);
			// hardware Micro Bend LED colours, relocated: blue forward, cyan
			// on an exact octave, green reversed, gold reversed-on-octave
			if (module && !module->macro) {
				float oct = module->uiMicroOct;
				bool rev = module->uiMicroRev;
				bool onOct = std::fabs(oct - std::round(oct)) < 0.03f;
				NVGcolor mc = rev ? (onOct ? nvgRGB(0xff, 0xc2, 0x3e) : nvgRGB(0x3f, 0xe0, 0x63))
				                  : (onOct ? nvgRGB(0x35, 0xd3, 0xe0) : nvgRGB(0x5a, 0x8d, 0xff));
				char mtxt[24];
				snprintf(mtxt, sizeof(mtxt), "M%+.2f%s", oct, rev ? "R" : "");
				nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
				nvgFillColor(vg, mc);
				nvgText(vg, mm2px(X1 - 0.6f), mm2px(Y0 + 2.6f), mtxt, NULL);
			}
		}
	}
};

// ---------------------------------------------------------------- widget ----
struct BadSectorWidget : ModuleWidget {
	// mm positions — keep in sync with gen_panel.py
	static constexpr float KX_L = 15.f, KX_R = 66.28f;
	static constexpr float KY1 = 25.f, KY2 = 46.5f, KY3 = 68.f;

	// context-menu slider bound to a module float
	struct FloatQ : Quantity {
		float* ptr; std::string name; float defVal;
		FloatQ(float* p, std::string n, float d) : ptr(p), name(n), defVal(d) {}
		void setValue(float v) override { *ptr = math::clamp(v, 0.f, 1.f); }
		float getValue() override { return *ptr; }
		float getDefaultValue() override { return defVal; }
		std::string getLabel() override { return name; }
		int getDisplayPrecision() override { return 2; }
	};

	BadSectorWidget(BadSector* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/BadSector.svg")));

		addChild(createWidget<BsScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<BsScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<BsScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<BsScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		BsChecksumArt* art = new BsChecksumArt();
		art->box.pos = Vec(0, 0);
		art->box.size = box.size;
		art->module = module;
		addChild(art);

		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(KX_L, KY1)), module, BadSector::BUFFER_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(KX_R, KY1)), module, BadSector::REPEAT_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(KX_L, KY2)), module, BadSector::MIX_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(KX_R, KY2)), module, BadSector::MICRO_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(KX_L, KY3)), module, BadSector::DAMAGE_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(KX_R, KY3)), module, BadSector::CVAMT_PARAM));

		// selectors with square lights + stored-value dots
		addParam(createParamCentered<BsSqButton>(mm2px(Vec(33.6f, 68.f)), module, BadSector::DMGSEL_PARAM));
		addChild(createLightCentered<BsSqLight>(mm2px(Vec(33.6f, 68.f)), module, BadSector::DMGSEL_LIGHT));
		addParam(createParamCentered<BsSqButton>(mm2px(Vec(47.7f, 68.f)), module, BadSector::CVSEL_PARAM));
		addChild(createLightCentered<BsSqLight>(mm2px(Vec(47.7f, 68.f)), module, BadSector::CVSEL_LIGHT));
		addChild(createLightCentered<TinyLight<CyanLight>>(mm2px(Vec(33.6f, 62.9f)), module, BadSector::DOT_DMG_LIGHT));
		addChild(createLightCentered<TinyLight<CyanLight>>(mm2px(Vec(47.7f, 62.9f)), module, BadSector::DOT_CV_LIGHT));

		// mode / clock / freeze
		addParam(createParamCentered<BsSqButtonSmall>(mm2px(Vec(34.f, 54.4f)), module, BadSector::MODE_PARAM));
		addChild(createLightCentered<BsSqLightSmall>(mm2px(Vec(34.f, 54.4f)), module, BadSector::MODE_LIGHT));
		addParam(createParamCentered<BsSqButtonSmall>(mm2px(Vec(40.64f, 54.4f)), module, BadSector::CLOCKBTN_PARAM));
		addChild(createLightCentered<BsSqLightSmall>(mm2px(Vec(40.64f, 54.4f)), module, BadSector::CLK_LIGHT));
		addParam(createParamCentered<BsSqButtonSmall>(mm2px(Vec(47.3f, 54.4f)), module, BadSector::FREEZE_PARAM));
		addChild(createLightCentered<BsSqLightSmall>(mm2px(Vec(47.3f, 54.4f)), module, BadSector::FRZ_LIGHT));

		// jacks — CV row, gate row, audio row
		static const float JX[6] = {10.2f, 22.86f, 35.52f, 48.18f, 60.84f, 73.5f};
		static const float CVY = 89.f, GATEY = 101.f, AUY = 116.5f;
		static const int cvIds[6] = {
			BadSector::BUFFER_CV_INPUT, BadSector::REPEAT_CV_INPUT, BadSector::MIX_CV_INPUT,
			BadSector::BEND_CV_INPUT, BadSector::BREAK_CV_INPUT, BadSector::CORRUPT_CV_INPUT
		};
		for (int i = 0; i < 6; i++)
			addInput(createInputCentered<BsPort>(mm2px(Vec(JX[i], CVY)), module, cvIds[i]));
		// gate jacks sit under their matching CV columns; FRZ takes the MIX column
		static const int gateIds[4] = {
			BadSector::FREEZE_GATE_INPUT, BadSector::BEND_GATE_INPUT,
			BadSector::BREAK_GATE_INPUT, BadSector::CORRUPT_GATE_INPUT
		};
		for (int i = 0; i < 4; i++)
			addInput(createInputCentered<BsPort>(mm2px(Vec(JX[i + 2], GATEY)), module, gateIds[i]));
		// bottom row on the same grid: audio pairs outside, clock/reset centred
		addInput(createInputCentered<BsPort>(mm2px(Vec(JX[0], AUY)), module, BadSector::IN_L_INPUT));
		addInput(createInputCentered<BsPort>(mm2px(Vec(JX[1], AUY)), module, BadSector::IN_R_INPUT));
		addInput(createInputCentered<BsPort>(mm2px(Vec(JX[2], AUY)), module, BadSector::CLOCK_INPUT));
		addInput(createInputCentered<BsPort>(mm2px(Vec(JX[3], AUY)), module, BadSector::RESET_INPUT));
		addOutput(createOutputCentered<BsPort>(mm2px(Vec(JX[4], AUY)), module, BadSector::OUT_L_OUTPUT));
		addOutput(createOutputCentered<BsPort>(mm2px(Vec(JX[5], AUY)), module, BadSector::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		BadSector* m = getModule<BadSector>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Corrupt effect",
			{"Decimate", "Dropout", "Destroy", "DJ Filter (extra)", "Vinyl Sim (extra)"}, &m->corruptSel));
		menu->addChild(createBoolPtrMenuItem("Original 3 corrupt effects only (hardware)", "", &m->originalCorruptOnly));
		menu->addChild(createBoolPtrMenuItem("Micro: reverse playback", "", &m->microRev));
		menu->addChild(createBoolPtrMenuItem("Micro: Break knob = silence (off = traverse)", "", &m->microSilence));
		menu->addChild(createBoolPtrMenuItem("MICRO knob active in Macro (global varispeed)", "", &m->microInMacro));
		menu->addChild(createBoolPtrMenuItem("Stereo: unique per channel", "", &m->stereoUnique));
		menu->addChild(createBoolPtrMenuItem("Gates: momentary (hold) instead of latching", "", &m->gatesMomentary));
		menu->addChild(createBoolPtrMenuItem("Freeze button: momentary", "", &m->freezeMomentary));
		auto addSlider = [&](float* ptr, const char* name, float def) {
			ui::Slider* s = new ui::Slider;
			s->quantity = new FloatQ(ptr, name, def);
			s->box.size.x = 200.f;
			menu->addChild(s);
		};
		addSlider(&m->windowing, "Glitch windowing", 0.02f);
		addSlider(&m->stereoWidth, "Stereo width", 0.f);
		addSlider(&m->ledBrightness, "LED brightness", 1.f);
		menu->addChild(createMenuItem("Clear buffer", "", [m]() {
			std::fill(m->bufL.begin(), m->bufL.end(), 0.f);
			std::fill(m->bufR.begin(), m->bufR.end(), 0.f);
		}));
		menu->addChild(createMenuItem("Restore default settings", "", [m]() { m->restoreDefaults(); }));
	}
};

Model* modelBadSector = createModel<BadSector, BadSectorWidget>("BadSector");
