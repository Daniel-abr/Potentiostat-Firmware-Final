/*
 * ad5941.h — SPI driver for the Analog Devices AD5941 potentiostat AFE,
 * configured for a polled (no on-chip sequencer) differential pulse
 * voltammetry (DPV) measurement: a staircase bias on the low power DAC
 * (LPDAC) with a superimposed pulse each step, sensed through the high
 * speed TIA (HSTIA) and read back via the ADC.
 *
 * SOURCING: every register address/bit field cited below with a page
 * number was read directly out of docs/ad5940-5941.pdf (Rev. G, 142 pages)
 * — either independently re-verified against the raw extracted datasheet
 * text, or taken from a research pass over the same PDF that was spot
 * checked against the primary text and matched exactly (marked "(agent)"
 * below where I did not personally re-verify the exact bit position).
 * Anywhere a page number is missing, the value is an ENGINEERING
 * ASSUMPTION, not a datasheet fact — those are called out explicitly and
 * should be verified against real hardware before trusting a measurement.
 *
 * TOPOLOGY (per Table 21, p.38, "Voltammetry (Full Power Pulse Test)
 * Using High Speed TIA" — the datasheet's own recommended config for a
 * pulsed voltammetry technique like DPV):
 *   - LPDAC's 12-bit output (VBIAS0) drives the potentiostat, which biases
 *     CE0 relative to RE0 — this is where the staircase+pulse waveform is
 *     generated, by writing successive DAC codes (no on-chip waveform
 *     generator supports a staircase; direct register writes are an
 *     explicitly first-class, documented alternative to the sequencer —
 *     p.6, p.54).
 *   - LPDAC's 6-bit output (VZERO0) is held fixed and routed to the HSTIA's
 *     noninverting input (HSTIACON VBIASSEL=01), setting the SE0 bias.
 *   - HSTIA senses the resulting current at SE0 and its output is read
 *     differentially by the ADC (MUXSELP/MUXSELN = HSTIA+/HSTIA-, p.57).
 *
 * NOT IMPLEMENTED: the on-chip sequencer/command-FIFO (confirmed optional
 * per the datasheet itself, p.6/p.54/p.89-92 — direct SPI register access
 * is a normal, supported mode) and the high speed DAC path (not needed;
 * DPV's staircase+pulse comes entirely from the LPDAC).
 */
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <math.h>

// Result of AD5941::autoRangeRtia() — see that method's doc comment. Kept as
// a free struct (like SweepParams) so main.cpp can hold one in SweepState
// without depending on AD5941's internals.
struct AfeRangeResult {
  uint8_t rtiaCode = 1;        // final HSRTIACON[3:0] code chosen (1=1kOhm .. 7=160kOhm)
  float   rtiaOhms = 1000.0f;  // == AD5941::rtiaOhmsForCode(rtiaCode), for convenience/display
  float   correction = 1.0f;   // ratiometric self-check factor; ALREADY applied to every
                                // reading AD5941::poll() reports for the sweep that follows
  float   peakProbeNa = 0.0f;  // corrected peak |current| seen at the final range while probing
  bool    atMaxRange = false;  // true only if ranging stopped because the 160kOhm ceiling was
                                // hit, not because the step-up threshold was cleared — the
                                // achieved range may still be underscale
  bool    aborted = false;     // true if autoRangeRtia()'s optional stopRequested() callback
                                // fired mid-probe (item A) — the other fields are whatever was
                                // reached so far and should NOT be used to start a sweep
};

class AD5941 {
public:
  AD5941(uint8_t csPin, uint8_t resetPin, uint8_t gpio0Pin)
    : _cs(csPin), _reset(resetPin), _gpio0(gpio0Pin) {}

  // Call once in setup(), before reset()/register access.
  void begin(int8_t sck, int8_t miso, int8_t mosi) {
    pinMode(_cs, OUTPUT);
    digitalWrite(_cs, HIGH);
    pinMode(_reset, OUTPUT);
    digitalWrite(_reset, HIGH);
    pinMode(_gpio0, INPUT);

    _spi.begin(sck, miso, mosi, _cs);
    // Mode 0 (CPOL=0/CPHA=0), MSB first — matches the datasheet's SPI
    // timing description (p.104: launched on SCLK falling edge, sampled
    // on rising edge). Max SCLK is 12.5MHz (p.104); running well under
    // that for margin — raise if you need faster register access.
    _settings = SPISettings(4000000, MSBFIRST, SPI_MODE0);
  }

  // Hardware reset + mandatory system init (Table 14, p.29) + power-up of
  // the blocks this driver uses (LPDAC, potentiostat, HSTIA, ADC). Returns
  // true if the chip ID register reads back the documented constant.
  bool reset();

  // Precompute the DPV staircase from the given sweep parameters. Call
  // once before startSweep(). pulseWidthMs is the datasheet's "modulation
  // time" (pulse duration, sampled near its end); intervalMs is the
  // "interval time" (total time budget per step — the difference,
  // intervalMs - pulseWidthMs, is spent at the base level before the
  // pre-pulse sample). equilibrationMs is a ONE-TIME hold at startV before
  // the first timed step begins (see startSweep()'s comment for why).
  // totalSteps() reports how many points the sweep will produce once
  // configured.
  //
  // Returns false (and refuses to configure — _phase stays IDLE) if
  // pulseWidthMs or (intervalMs - pulseWidthMs) is too short to fit a full
  // Ibefore/Iend averaging window plus settling margin — see
  // kSinc2WindowMs/kSettlingMarginMs below. SweepLimits (sweep_params.h)
  // permits pulse widths/base dwells shorter than this floor, so this is a
  // real, reachable failure, not a theoretical one; per explicit
  // instruction, the caller must refuse to start rather than silently
  // truncating the window or changing the requested timing.
  bool configureSweep(float startV, float endV, float stepV, float pulseAmpV,
                       float pulseWidthMs, float intervalMs, float equilibrationMs);
  int totalSteps() const { return _totalSteps; }

  // Applies the first step's base bias and begins the per-step timing
  // state machine that poll() advances.
  void startSweep();

  // Halts the sequencer state machine immediately and returns the LPDAC
  // to a safe idle bias (VBIAS0 = VZERO0, i.e. ~0V cell bias) — does not
  // power down the AFE blocks, so a new sweep can start right away.
  void stopSweep();

  // Call every loop() tick while a sweep is running (mirrors main.cpp's
  // existing non-blocking pacing pattern — see serviceSweep()). Advances
  // the base-settle -> sample -> pulse -> sample state machine using
  // millis(); returns true exactly once per step, the moment a new DPV
  // point (outV, outDeltaINa = Iend - Ibefore) is ready. Returns false on
  // every other call (still mid-step, or the sweep isn't running/is done).
  bool poll(float &outV, float &outDeltaINa);

  bool isSweepDone() const { return _step >= _totalSteps; }

  // Bench self-test: exercises the LPDAC -> potentiostat -> ADC path with NO
  // electrode or cell attached. Puts the potentiostat in unity-gain
  // follower mode (adds SW10 to the pulse-test switch routing), commands
  // VBIAS0 to a handful of known voltages, and reads the CE0 pin back
  // through the ADC (MUXSELP=0b011001, p.63), comparing against the
  // commanded voltage. A pass proves SPI communication, LPDAC programming,
  // the potentiostat stage, and the ADC+PGA read path. Prints register
  // readbacks, each point and a final PASS/FAIL to Serial; returns true
  // only if every point is within tolerance. Temporarily reconfigures
  // LPTIASW0/ADCCON and restores the normal config afterward — only call
  // while idle (reset() must have already run; don't call mid-sweep).
  // The BUF_VREF1V8 -> RE0 jumper must be OFF (SW10 ties RE0 to CE0).
  bool selfTestDacAdcLoopback();

  // ---- Runtime-selectable RTIA (see autoRangeRtia() below) ----
  uint8_t currentRtiaCode() const { return _rtiaCode; }
  float currentRtiaOhms() const { return rtiaOhmsForCode(_rtiaCode); }
  // True if the most recently *completed* poll() point (both its Ibefore and
  // Iend samples) stayed under kOverloadFraction of the ADC's usable input
  // window for the RTIA active at the time. main.cpp checks this once per
  // poll()==true during a real sweep and aborts if false. This project's own
  // safety policy, not something the AD5941 hardware itself flags.
  bool lastSampleOverloaded() const { return _lastSampleOverloaded; }
  // Ibefore/Iend of the most recently *completed* poll() point (nA,
  // corrected — same values ΔI was computed from) — for CSV/WS logging
  // that needs them individually, not just their difference (item 5).
  float lastIBeforeNa() const { return _iBeforeNa; }
  float lastIEndNa() const { return _lastIEndNa; }

  // Safety-first pre-sweep auto-ranging probe — this project's own
  // engineering design (confirmed with the user), not a datasheet procedure.
  // Synchronous/blocking (a few hundred ms to a couple of seconds depending
  // on numProbeVolts and how many codes it steps through). Call once from
  // main.cpp's startSweep(), after reset() has already run, BEFORE
  // configureSweep()/startSweep() — same calling convention/idle
  // precondition as selfTestDacAdcLoopback(). Reuses writeLpdacCodes()/
  // readHstiaCurrentNa() directly (a plain DC bias + one current sample per
  // probe point, no pulse) and never touches _phase/_step, so it can't
  // corrupt poll()'s state machine no matter when it's called.
  //
  // Algorithm: start at RTIA=1kOhm (the datasheet's own IMAX=0.9V/RTIA
  // sizing guidance, p.36-37, keeps max current under ~900uA there, safely
  // under a typical potentiostat's overcurrent limit even before anything is
  // known about the sample). Probe every voltage in probeVolts with a short
  // settle and no pulse, track the peak |current|. If that peak is under
  // kAutoRangeStepUpThresholdPct of the active range's full scale, step
  // RTIA up one code and re-probe the SAME potentials (never step down);
  // repeat until either the threshold clears or the 160kOhm ceiling is hit.
  //
  // Ratiometric self-check (the lighter alternative to RCAL/HSDAC-based
  // hardware gain calibration the user chose — see ad5941.cpp for why):
  // at each step-up, the true current at a given probe potential should
  // read the same on both the old and new range. Chains a running
  // `correction` factor from that ratio, starting at 1.0 at the initial
  // 1kOhm range. This makes the final reading *self-consistent relative to
  // the 1kOhm range*, NOT absolutely accurate — there is no external
  // reference (like RCAL) involved, so any error common to all ranges (e.g.
  // a shared ADC offset) is not corrected. Flag this clearly to the user;
  // do not present it as a true calibration.
  //
  // probeVolts are relative-to-VZERO0 volts, same convention as
  // SweepParams' startV/endV. Leaves HSRTIACON at the finally-chosen code
  // and the LPDAC idled at 0V-relative bias (same safe-idle state
  // stopSweep() leaves it in).
  //
  // stopRequested (item A), if given, is polled between probe steps (never
  // mid-window — this is a plain delay()-separated loop, not part of the
  // DPV pulse timing) so a STOP press during this ~multi-second blocking
  // probe can abort it early instead of being silently swallowed. A plain
  // C function pointer, not std::function, matching this project's other
  // callback conventions (AppContext) — on firing, the probe stops as soon
  // as it notices, idles the LPDAC same as a normal finish, and returns
  // with AfeRangeResult::aborted=true so the caller knows not to proceed.
  AfeRangeResult autoRangeRtia(const float *probeVolts, int numProbeVolts,
                                bool (*stopRequested)() = nullptr);

  // ---- Debug output for the windowed Ibefore/Iend averaging (off by
  // default — this roughly doubles poll()'s existing per-step Serial
  // logging, so it's opt-in rather than always-on). When enabled, poll()
  // additionally prints, for both the Ibefore and Iend window of every
  // step: how many sinc2 samples were actually captured (should equal
  // kSinc2WindowSamples every time; less means the ready-flag wait timed
  // out — see sampleSinc2Window()), the mean raw ADC code, and the
  // within-window standard deviation (raw code units) — a direct read on
  // how noisy the sinc2-filtered signal still is before any further
  // processing.
  void setDebugWindows(bool on) { _debugWindows = on; }
  bool debugWindows() const { return _debugWindows; }

  // One-off GAIN-ONLY measurement against a known resistor (this project's
  // own design, confirmed with the user — NOT a datasheet procedure, and
  // deliberately lighter than true RCAL-based hardware calibration, which
  // this driver skips entirely for now). Forces RTIA to rtiaCode (does not
  // use whatever's currently active — which range gets calibrated is the
  // whole point of the call), biases to two potentials kCalibrationDeltaV
  // apart, averages the HSTIA current at each with the same fixed-count
  // sinc2 window poll() uses, and derives a GAIN factor from the CURRENT
  // DIFFERENCE (deltaI/deltaV vs 1/knownOhms) rather than a single-point
  // V/R reading — any DAC/VZERO0 or ADC offset is approximately constant
  // across the two bias points and cancels out of that difference,
  // leaving only the gain error. This method itself still computes gain
  // only, by design (differencing is what makes it insensitive to offset)
  // — it does NOT derive a value for kOffsetTable_nA below. If you also
  // want an offset estimate for a range, use calPoint() at >=2 known E
  // values against a known resistor and solve the 2-point line yourself
  // (see calPoint()'s doc comment) — the two tools are complementary, not
  // redundant. Prints the derived gain factor; does NOT write it anywhere
  // — kGainTable below is a hand-edited constant, by design, so you copy
  // the printed number in yourself after checking it looks sane.
  // Synchronous/blocking; only call while idle (same precondition as
  // autoRangeRtia()/selfTestDacAdcLoopback()).
  void calibrateGain(float knownOhms, uint8_t rtiaCode);

  // Raw single-point diagnostic (confirmed with the user) — fixes RTIA to
  // rtiaCode (via useFixedRtia(): auto-ranging off, CTIA bits kept per
  // kCtiaConBits), holds a static E (corrected sign convention, via
  // holdStaticE()), waits 3s to settle, then averages ~2s worth of sinc2
  // samples (round(2000ms / kSinc2SamplePeriodMs) = 1798 samples — reuses
  // the same sampleSinc2Window() primitive as everything else, just with a
  // much larger count; no edge-alignment requirement here unlike the DPV
  // windows, so a plain large fixed count is simplest). Prints the RAW
  // (uncorrected — kGainTable/kOffsetTable_nA are deliberately NOT applied
  // here, same circularity reasoning as readHstiaCurrentNa()) mean current
  // in nA and its standard deviation in nA. Leaves E held afterward (does
  // NOT reset to 0V) so you can keep probing with a DMM/meter right after
  // the printed result — call holdStaticE(0) or stopSweep() to release.
  //
  // To derive gain+offset for a range: run this at >=2 different E values
  // against a known resistor (expected raw current at each E is E/knownOhms
  // — Ohm's law, using the corrected sign convention's E), then solve the
  // 2-point line: gain = (Iexp2-Iexp1)/(rawNa2-rawNa1), offset =
  // Iexp1 - gain*rawNa1. Pasting E=0 as one of the two points is
  // convenient — Iexp is then exactly 0, so offset = -gain*rawNa(E=0).
  // Synchronous/blocking (~5s: 3s settle + ~2s capture); only call while
  // idle.
  void calPoint(uint8_t rtiaCode, float requestedE);

  // ---- Fixed (non-auto-ranged) RTIA selection ----
  // Forces RTIA to the given code and resets the ratiometric self-check
  // factor to neutral (1.0), since no probing pass has run to compute one —
  // used by main.cpp when SweepParams::autoRange is false, in place of
  // autoRangeRtia(). Public (unlike the private setRtiaCode() it wraps)
  // because choosing a fixed range is now a normal, user-facing operation,
  // not just an internal implementation detail of auto-ranging/reset().
  void useFixedRtia(uint8_t code);

  // ---- Static bias hold (bench test — confirmed with the user) ----
  // Commands VBIAS0/VZERO0 for a fixed E and returns, with NO pulse and NO
  // sweep/timing state touched — for probing E's polarity with a DMM (e.g.
  // "does E become more positive at RE0 when I ask for a larger E?"). Only
  // call while idle (same precondition as the other synchronous helpers
  // above). Call stopSweep() or holdStaticE(0) to release back to idle.
  void holdStaticE(float requestedE);

private:
  // EQUILIBRATE: a one-time hold at startV before the first step's own
  // BASE_SETTLE begins — see startSweep()'s comment for why this exists
  // separately from the per-step dwell. Non-blocking, like every other
  // phase here: poll() just returns false (same as BASE_SETTLE/
  // PULSE_SETTLE) until the hold elapses, keeping loop() free to service
  // WiFi/TFT/input during what could be several seconds.
  enum class Phase { IDLE, EQUILIBRATE, BASE_SETTLE, PULSE_SETTLE };

  // ---- Register-width-aware SPI transactions (p.104-106) ----
  // Two-transaction protocol: SETADDR (0x20 + 16-bit address) as its own
  // CS-low/high cycle, then WRITEREG (0x2D) or READREG (0x6D, +1 dummy
  // byte before the data phase) as a second CS-low/high cycle. This
  // replaced an earlier placeholder guess that used the wrong opcodes —
  // 0x20/0x2D/0x6D are the actual documented SPI command bytes (Table 126,
  // p.104), not inferred.
  void spiSetAddr(uint16_t addr);
  void writeReg(uint16_t addr, uint32_t value, uint8_t nBytes);
  uint32_t readReg(uint16_t addr, uint8_t nBytes);
  void writeReg16(uint16_t addr, uint16_t v) { writeReg(addr, v, 2); }
  void writeReg32(uint16_t addr, uint32_t v) { writeReg(addr, v, 4); }
  uint16_t readReg16(uint16_t addr) { return (uint16_t)readReg(addr, 2); }
  uint32_t readReg32(uint16_t addr) { return readReg(addr, 4); }

  // ---- LPDAC bias-code math (p.30-31) ----
  // 12-bit VBIAS0: 0.2V (code 0) .. 2.4V (code 0xFFF), 1 LSB = 537.2uV.
  // 6-bit VZERO0: 0.2V (code 0) .. 2.366V (code 0x3F), 1 LSB = 34.375mV.
  // Effective cell bias = VBIAS0 - VZERO0.
  //
  // ASSUMPTION (not in the datasheet — this is this project's own
  // engineering choice, not a chip fact): VZERO0 is held fixed at
  // kVZero0Volts so the whole 0.2-2.4V VBIAS0 range is available to sweep
  // the *relative* cell bias requested by SweepParams (startV/endV, which
  // main.cpp treats as volts relative to this fixed point, matching how
  // a real potentiostat presents "electrode potential"). Set to 1.7125V
  // (6-bit code 44 exactly: 0.2 + 44*34.375mV = 1.7125V) — chosen with the
  // user to leave >=0.2V margin from both LPDAC rails at the sweep's
  // extremes, including the pulse amplitude (see SweepLimits, which must
  // stay in sync with this). Verified against Table 1 (p.9-11): HSTIA
  // input common-mode range is 300mV..AVDD-700mV and output range is
  // 200mV..AVDD-400mV; at AVDD=3.3V (this chip's own AVDD pin spec — ASSUMED,
  // not read back from hardware) that's [0.30V,2.60V] input / [0.20V,2.90V]
  // output, and VZERO0=1.7125V +/- the 0.9V HSTIA swing (kAdcInputWindowVolts
  // below, itself matching Table 1's own "<+/-900mV" HSTIA guidance) needs
  // [0.8125V,2.6125V] of output range — fits with 0.61V/0.29V to spare.
  // This margin requires AVDD to be close to its nominal 3.3V, not the
  // datasheet's 2.8V floor (at 2.8V the output ceiling is only 2.4V,
  // clipping before 2.6125V) — verify actual AVDD with a meter if unsure.
  static constexpr float kVZero0Volts = 1.7125f;
  static constexpr float kDacMinVolts = 0.2f;
  static constexpr float k12BitLsbVolts = 2.2f / 4095.0f;   // 537.2uV, p.31 Eq.3
  static constexpr float k6BitLsbVolts = 34.375e-3f;         // p.31 Eq.4
  // SIGN CONVENTION (confirmed with the user, and directly supported by the
  // datasheet's own wording, p.31 "Electrochemical Amperometric Measurement":
  // "The bias voltage on the sensor effectively becomes the difference
  // between the 12-bit output and the 6-bit output", i.e. the chip's native
  // quantity is VBIAS0-VZERO0 — the NEGATIVE of the standard electrochemistry
  // convention E(WE vs RE) = V(SE0)-V(RE0) = VZERO0-VBIAS0, since VBIAS0
  // biases the CE0/RE0 side and VZERO0 biases the SE0 (working/sense
  // electrode) side. vbias0Code() takes E in this STANDARD sense and flips
  // the sign internally: VBIAS0 = VZERO0 - E. A positive DPV pulse (E
  // increasing) therefore DECREASES the commanded VBIAS0 — this is handled
  // automatically by every caller that already adds a positive pulse
  // amplitude to a base E value (poll(), autoRangeRtia(), calibrateGain());
  // none of them needed to change.
  uint16_t vbias0Code(float requestedE) const;
  uint16_t vzero0Code() const;
  // Returns the FINAL 12-bit code actually written to LPDACDAT0[11:0] —
  // i.e. code12 after the loading-effect correction below (a decrement, at
  // most 1 LSB) — so callers can compute the true applied E from what the
  // chip actually holds, not what was requested (see poll()'s _baseCode12
  // use and item 2 of the sign-convention fix). code6 (VZERO0) is never
  // adjusted by this correction, so its own value is always what's written.
  uint16_t writeLpdacCodes(uint16_t code12, uint16_t code6);
  // Converts a WRITTEN (already loading-corrected) 12-bit code, plus the
  // written 6-bit code, into the actual applied E — the datasheet's own
  // Eq.5/6 (p.31: "12-Bit/6-Bit DAC Output Voltage = 0.2V + code*LSB")
  // applied to whatever ended up in the register, then combined per the
  // sign convention above. Using the plain (non-"-LSB") form is correct
  // here because writeLpdacCodes()'s -1 adjustment already pre-compensates
  // the stored code12 so a direct per-Eq.5/6 readback matches the real
  // output in both the "12-bit < 6-bit" and "12-bit >= 6-bit" cases (p.31).
  float appliedE(uint16_t writtenCode12, uint16_t writtenCode6) const;

  // ---- ADC trigger/read + current conversion (p.54-57) ----
  // Blocks (briefly, bounded by kAdcTimeoutUs) waiting for the ADC-ready
  // flag — the datasheet gives no cycle-accurate conversion latency for
  // this polled (non-sequencer) usage, so this is a bounded busy-wait,
  // not an assumption-free datasheet number. At DPV's step rate (tens of
  // ms) this is negligible next to the dwell times.
  // ADCDAT is an offset-binary code centered on 0x8000 (p.56 Eq.13: (ADCDAT
  // - 0x8000)/2^15), NOT two's complement — must stay unsigned through the
  // read path so values above 0x8000 aren't misread as negative directly.
  // If readyOut is given it is set to whether every expected ADC-result
  // flag arrived in time; on a timeout the raw ADCDAT is still returned
  // (rather than a silent 0) so a stuck ADC is diagnosable.
  uint16_t triggerAndReadAdcCounts(bool *readyOut = nullptr);
  // counts is a float here (not uint16_t) so a fractional window MEAN can
  // be converted directly — the offset-binary formula (p.56 Eq.13) is
  // linear, so applying it to an already-averaged value is equivalent to
  // averaging the converted volts of every individual sample.
  float adcCountsToVolts(float counts) const;
  float readHstiaCurrentNa();

  // ---- Windowed sinc2-filter averaging (Ibefore/Iend — replaces the
  // single-shot triggerAndReadAdcCounts()/readHstiaCurrentNa() pair above
  // for real sweep sampling; those two stay as-is and are still used by
  // autoRangeRtia()/selfTestDacAdcLoopback(), which only need one quick
  // reading, not a noise-reduced average). See ad5941.cpp's poll() for the
  // full rationale: the 50/60Hz notch filter is bypassed (LPFBYPEN=1, see
  // reset()) and replaced with this explicit post-hoc average over a fixed
  // number of raw sinc2 filter results, timed to land entirely within the
  // settled portion of the base/pulse dwell. ----
  struct Sinc2Window {
    float meanCounts = 0.0f;
    float stdDevCounts = 0.0f;
    int sampleCount = 0;
    bool timedOut = false; // true if the Sinc2-ready flag (INTCFLAG0 bit2) didn't
                            // assert within kAdcTimeoutUs for at least one sample —
                            // sampleCount may be less than requested in that case
  };
  // Blocking: takes ~numSamples * kSinc2SamplePeriodMs (~20ms for
  // kSinc2WindowSamples) to return. Assumes the caller has already left
  // ADCCONVEN=1 (continuous conversion) — see startSweep()/stopSweep() for
  // the real-sweep case, or calibrateGain()'s own bracketing — so this
  // function only reads SINC2DAT results as they arrive rather than
  // starting/stopping a conversion per sample (starting/stopping per
  // sample would reintroduce a settling transient the datasheet gives no
  // number for, on every single sample instead of once per window).
  Sinc2Window sampleSinc2Window(int numSamples);
  // Wraps sampleSinc2Window() with the ADCCONVEN-on bracketing, mean-counts
  // -> volts -> nA conversion, and the same overload check
  // readHstiaCurrentNa() does. Raw — no calibration applied, same reasoning
  // as readHstiaCurrentNa(). outWindow, if given, receives the raw window
  // stats for debugWindows() logging.
  float readHstiaCurrentNaWindowed(Sinc2Window *outWindow = nullptr);
  // I_corrected = kGainTable[code]*rawNa + kOffsetTable_nA[code] — see
  // those tables' comment for the full rationale. This is now the ONLY
  // calibration applied to a reading; the automatic ratiometric self-check
  // (_correction, from autoRangeRtia()) is still computed and reported
  // (AfeRangeResult::correction) as a diagnostic, but is no longer
  // consulted here (confirmed with the user — replaces the previous
  // either/or logic against a gain-only table).
  float applyCalibration(float rawNa, uint8_t code) const;

  // ---- RTIA gain resistor — runtime-selectable via autoRangeRtia(), see
  // the public method above. HSRTIACON[3:0] codes 0-7 (codes 8-15 are
  // "RTIA open", unusable), Table 38, p.52. ----
  static constexpr float kRtiaOhmsTable[8] = {
    200.0f, 1000.0f, 5000.0f, 10000.0f, 20000.0f, 40000.0f, 80000.0f, 160000.0f
  };
  static constexpr uint8_t kRtiaCodeMin = 0, kRtiaCodeMax = 7;
  static float rtiaOhmsForCode(uint8_t code) { return kRtiaOhmsTable[constrain(code, kRtiaCodeMin, kRtiaCodeMax)]; }

  // HSRTIACON[12:5] CTIACON: stabilization capacitor in parallel with RTIA
  // (Table 38, p.52). NOT a plain binary count — each bit selects a
  // capacitor to add in parallel: bit0=1pF, bit1=2pF, bit2=4pF, bit3=8pF,
  // bit4=16pF; bits 5 and 6 ("32pF"/"64pF") are explicitly listed as
  // "Not used" in this datasheet revision's Table 38 — 0x3F (bits[5:0]
  // combined) sets bit5 (the documented-invalid "32pF" bit) on top of the
  // fully-documented bits[4:0]. Set to 0x3F anyway per explicit
  // instruction (confirmed twice, after this discrepancy was flagged) —
  // if HSTIA readings look wrong/noisy in a way traceable to this cap,
  // this bit5 setting is the first thing to revisit.
  static constexpr uint32_t kCtiaConBits = 0x3FUL << 5;

  // Per-range affine calibration, PASTE VALUES HERE: I_corrected =
  // kGainTable[code]*I_raw + kOffsetTable_nA[code]. This pair is the ONLY
  // calibration applied to a reading (see applyCalibration()) — the
  // ratiometric _correction from autoRangeRtia() is no longer consulted at
  // all (confirmed with the user). Index [0..7] = HSRTIACON RTIA code
  // (0=200ohm .. 7=160kohm, kRtiaOhmsTable).
  //
  // CALIBRATED 2026-09-28: two-point calibration (calPoint() at +/-0.5V
  // against DMM-measured resistors: 4.59k, 38.5k, 81.5k, 146.7k — one
  // resistor per calibrated range below, giving one (gain, offset) pair
  // per range from the two E points). Check-point errors after applying
  // the fit (measured vs expected corrected current, at a held-out E):
  // code 2 (5k): -0.16%, code 5 (40k): +0.02%, code 6 (80k): +0.13%,
  // code 7 (160k): +0.38%.
  //
  // CALIBRATED 2026-10-11: same two-point method (calPoint() at +/-0.5V),
  // check-point at +0.25V, against DMM-measured resistors for the
  // remaining four ranges: codes 0 and 1 = 994 ohm, code 3 = 9.93k,
  // code 4 = 18.74k. Check-point errors: code 0 +0.44%, code 1 +0.40%,
  // code 3 -0.05%, code 4 +0.06%. This replaces code 3's earlier
  // 2026-10-10 single-point sweep-ΔI correction (0.8803407/0.0) with a
  // proper two-point measurement. (Code 4's gain from this round,
  // 0.9576104, later turned out to be an outlier — see 2026-10-17 below.)
  //
  // RECALIBRATED 2026-10-17: code 4 (20k) redone — two-point calPoint() at
  // +/-0.5V against a DMM-measured 19.9k resistor (nominal 20k/22k),
  // check-point at +0.25V: error 0.057%. This replaces the 2026-10-11
  // value (0.9576104), which sat well outside every other range's gain
  // (0.8971220-0.9191204) and was flagged pending cross-check — the new
  // value (0.9017899) falls right back in that band, resolving the flag.
  //
  // Code 0 (200ohm) was only calibrated at ~11% of that range's full
  // scale — the potentiostat amplifier is limited to +/-750uA in normal
  // mode (IBOOST=00, LPTIACON0 — see that register's bit table), so code 0
  // is not usable above ~750uA without switching to high-current mode
  // (not implemented here).
  static constexpr float kGainTable[8] = {
    0.9013030f,  // 0: 200 ohm  (calibrated — see 750uA note above)
    0.9140572f,  // 1: 1 kohm   (calibrated)
    0.9088862f,  // 2: 5 kohm   (calibrated)
    0.8971220f,  // 3: 10 kohm  (calibrated)
    0.9017899f,  // 4: 20 kohm  (recalibrated 2026-10-17)
    0.9128699f,  // 5: 40 kohm  (calibrated)
    0.9158477f,  // 6: 80 kohm  (calibrated)
    0.9191204f   // 7: 160 kohm (calibrated)
  };
  static constexpr float kOffsetTable_nA[8] = {
    160.04f, 278.66f, -69.54f, -81.571f, -15.766f, -9.159f, -3.014f, 1.005f
  };
  // Read-modify-write HSRTIACON[3:0] only (preserves CTIACON/TIASW6CON).
  // Used by reset() (power-up default) and autoRangeRtia() (the value that
  // actually matters for a real sweep) — nothing outside the class needs to
  // pick a range directly, so this stays private.
  void setRtiaCode(uint8_t code);

  // ADC's usable +/-0.9V differential input window at PGA=1 or 1.5 (the
  // datasheet's own RTIA-sizing guidance, p.36-37 Eq.9 — stated for the
  // LPTIA section but already relied on for the HSTIA path too in
  // selfTestDacAdcLoopback()'s test-point comments; NOT the ADC's raw code
  // range, which is wider per Eq.13 — this is the recommended linear
  // window). Used for both the auto-ranging full-scale math and the
  // overload check.
  static constexpr float kAdcInputWindowVolts = 0.9f;
  // This project's own safety margin (confirmed with the user): a sample
  // whose pre-division volts exceeds this fraction of kAdcInputWindowVolts
  // aborts the sweep in progress. Not a datasheet number.
  static constexpr float kOverloadFraction = 0.80f;
  // This project's own auto-ranging heuristic (confirmed with the user):
  // step RTIA up one code if the probed peak is under this percent of the
  // active range's full scale. Not a datasheet number.
  static constexpr float kAutoRangeStepUpThresholdPct = 10.0f;
  // Floor on the denominator in the ratiometric correction chain so a
  // near-zero re-probe reading can't blow the factor up — this project's
  // own numerical safeguard, not a datasheet number.
  static constexpr float kEpsilonNa = 0.01f;
  // ASSUMPTION (no datasheet number for either): settle time after
  // changing HSRTIACON (analog TIA feedback-loop settling) and after a
  // probe bias change (LPDAC + cell settling) during auto-ranging.
  // HYPOTHESIS, unverified: a real 100kOhm dummy load produced a first-probe
  // reading high enough that ranging never stepped up from 1kOhm at all,
  // which looks like a charging-current transient (I=C*dV/dt right after
  // the bias jumps) that the previous 20ms didn't wait out, rather than the
  // true ~10uA steady-state current such a load should show. Widened both
  // numbers as a first attempt — check the new per-probe Serial log
  // (autoRangeRtia() now prints every sample) to see whether this actually
  // fixed it or the real cause is something else.
  static const unsigned long kRtiaSwitchSettleMs = 20;
  static const unsigned long kProbeBiasSettleMs = 100;

  static constexpr float kAdcVref = 1.835f;   // p.56 Eq.13 — factory-calibrated constant for PGA=1.5x
  static constexpr float kPgaGain = 1.5f;     // chosen with the user: the only factory-calibrated PGA setting

  // ---- Ibefore/Iend windowed sinc2 averaging (replaces single-shot
  // ADCDAT reads for real sweep sampling — see poll()) ----
  //
  // The 50Hz/60Hz notch filter's own extra computation is bypassed
  // (ADCFILTERCON LPFBYPEN=1, see reset()) in favor of reading the plain
  // sinc2-filter output directly and averaging it ourselves over a fixed
  // window per sample. This is INDEPENDENT of AFECON Bit16 (SINC2EN,
  // p.28): that bit's own description is explicit that clearing it
  // "disables sinc2" outright, not just the notch's extra math on top of
  // an always-running sinc2 stage — an initial misreading here left it at
  // its reset default of 0, which powers down the whole sinc2 block, so
  // SINC2DAT/its ready flag never do anything (confirmed on real hardware:
  // every averaging window timed out with 0/18 samples on the very first
  // real sweep tested). SINC2EN is now included in AFECON_BASE (ad5941.cpp)
  // so the sinc2 stage stays active; LPFBYPEN is what then bypasses only
  // the notch's own additional filtering layered on top of sinc2's output.
  //
  // Sinc2 output rate at the filter config reset() leaves unchanged
  // (SINC3OSR=5, SINC2OSR=178 — ADCFILTERCON's reset value 0x301 already
  // encodes exactly this combination in bits[13:8]=0b000011, Table 41/43,
  // p.57-59) = 160kHz sinc3 rate / 178 = 898.876Hz, i.e. a period of
  // exactly 178/160000 s = 1.1125ms (160000 * 0.0011125 == 178 exactly).
  // This exact OSR pair is kept unchanged (rather than picked for a
  // rounder sample count) because it's the ONLY sinc3/sinc2 combination
  // the datasheet gives a characterized settling time for (Table 3, p.31)
  // — picking a different OSR to land on an exact whole-sample-count
  // window would mean guessing an uncharacterized settling time instead.
  static constexpr float kSinc2SamplePeriodMs = 1.1125f;
  // Fixed sample count per averaging window — NOT wall-clock bounded.
  // Explicit decision (confirmed with the user): a fixed count keeps every
  // window the same duration and close to a whole number of 50Hz mains
  // cycles (18 * 1.1125ms = 20.025ms vs the 20ms mains period, a 0.125%
  // deviation — negligible for mains rejection), where a wall-clock bound
  // would let the sample count vary by +/-1 depending on phase alignment,
  // both sitting off the 50Hz null and adding point-to-point variation in
  // exactly the noise metric this fix is trying to reduce.
  static const int kSinc2WindowSamples = 18;
  static constexpr float kSinc2WindowMs = kSinc2WindowSamples * kSinc2SamplePeriodMs; // 20.025ms
  // Minimum settling margin required between a bias-step edge (base->pulse
  // transition, or the pulse edge itself) and the START of the averaging
  // window that follows it, on top of the window itself. The datasheet's
  // own settling time for this exact filter config WITHOUT the notch is
  // 2.245ms (Table 3 footnote 1, p.31); this is rounded up generously
  // (more than 2x) to also absorb the software-timing jitter inherent in
  // poll()'s millis()-based phase-boundary scheduling (see poll()'s
  // comment) — not itself a datasheet number.
  static constexpr float kSettlingMarginMs = 5.0f;

  // ---- Self-test constants ----
  // VBIAS_CAP's typical voltage (p.56: "VBIAS_CAP is the voltage of the
  // VBIAS_CAP pin, typically 1.11V") — the conversion formula adds this
  // term whenever MUXSELN selects the VBIAS_CAP channel (code 0x8, p.56),
  // which the self-test's readback config does.
  static constexpr float kVBiasCapVolts = 1.11f;
  // ASSUMPTION (not a datasheet number): how far measured can be from
  // commanded before selfTestDacAdcLoopback() calls a point a FAIL. 50mV
  // is generous next to the DAC's own 537uV LSB — loose enough to absorb
  // VBIAS_CAP's "typically" tolerance and ADC noise, tight enough to catch
  // a genuinely broken signal path. Tighten once you have real hardware
  // numbers to calibrate against.
  static constexpr float kSelfTestToleranceV = 0.05f;

  // ---- Sweep state ----
  float _startV = 0, _endV = 0, _stepV = 0, _pulseAmpV = 0;
  unsigned long _pulseWidthMs = 0, _baseSettleMs = 0, _equilibrationMs = 0;
  int _step = 0, _totalSteps = 0;
  Phase _phase = Phase::IDLE;
  unsigned long _phaseStartMs = 0;
  float _iBeforeNa = 0;
  float _lastIEndNa = 0; // latched by poll() once a point completes — see lastIEndNa()
  // Final (loading-effect-corrected) 12-bit code actually written for the
  // CURRENT step's base level — captured from writeLpdacCodes()'s return
  // value so poll() can report the true applied E (item 2 of the sign
  // convention fix) instead of the requested, pre-quantization value.
  uint16_t _baseCode12 = 0;
  // Fixed, signed 12-bit LSB delta representing the pulse amplitude —
  // computed ONCE in configureSweep() from _pulseAmpV, not re-derived per
  // step. poll() computes each step's pulse code as _baseCode12 -
  // _pulseDeltaLsb (a plain integer subtraction, clamped to [0,4095]) so
  // the pulse amplitude is byte-for-byte identical every step, rather than
  // independently rounding the absolute pulse target (baseV+pulseAmpV)
  // each time, which could vary by +/-1 LSB step to step depending on
  // baseV's own fractional LSB remainder. Negative for a negative pulse
  // amplitude (E decreases during the pulse instead of increasing).
  int _pulseDeltaLsb = 0;

  // ---- RTIA range + overload state ----
  uint8_t _rtiaCode = 1;              // 1kOhm default — see reset()
  float _correction = 1.0f;           // set by autoRangeRtia(); reported via AfeRangeResult
                                       // as a diagnostic only — no longer consulted by
                                       // applyCalibration() (see kGainTable/kOffsetTable_nA)
  bool _lastReadOverloaded = false;   // set by every readHstiaCurrentNa() call (raw, unlatched)
  bool _stepOverloaded = false;       // OR of both samples in the in-progress DPV step
  bool _lastSampleOverloaded = false; // latched by poll() once a point completes

  bool _debugWindows = false; // see public setDebugWindows()

  uint8_t _cs, _reset, _gpio0;
  SPIClass _spi{FSPI};
  SPISettings _settings;

  // ---- Register map (addresses are the low 16 bits actually sent over
  // SPI — the datasheet prints them as 8-hex-digit MMR addresses, e.g.
  // 0x00002000, but confirms only the low 16 bits go on the wire, p.89) ----
  static const uint16_t REG_ADIID       = 0x0400; // p.28, 16-bit, expect 0x4144
  static const uint16_t REG_CHIPID      = 0x0404; // p.28, 16-bit
  static const uint16_t REG_AFECON      = 0x2000; // p.25, 32-bit
  static const uint16_t REG_PMBW        = 0x22F0; // p.26, 32-bit
  static const uint16_t REG_LPDACDAT0   = 0x2120; // p.34, 32-bit
  static const uint16_t REG_LPDACSW0    = 0x2124; // p.33, 32-bit
  static const uint16_t REG_LPDACCON0   = 0x2128; // p.32, 32-bit
  static const uint16_t REG_LPTIASW0    = 0x20E4; // p.39, 32-bit — confirmed directly in datasheet text
  static const uint16_t REG_LPTIACON0   = 0x20EC; // p.39 (Table 22 summary line), 32-bit
  static const uint16_t REG_HSRTIACON   = 0x20F0; // p.52 (Table 37 summary line), 32-bit
  static const uint16_t REG_DE0RESCON   = 0x20F8; // p.52, 32-bit
  static const uint16_t REG_HSTIACON    = 0x20FC; // p.52, 32-bit
  static const uint16_t REG_SWCON       = 0x200C; // p.76-77 (Tables 82/83), 32-bit, reset 0x0000FFFF (everything open)
  static const uint16_t REG_ADCCON      = 0x21A8; // p.62, 32-bit
  static const uint16_t REG_ADCFILTERCON= 0x2044; // p.57/59 (Table 42/43), 32-bit, reset 0x00000301
  static const uint16_t REG_ADCDAT      = 0x2074; // p.59, width inferred 32-bit (see .cpp note)
  static const uint16_t REG_SINC2DAT    = 0x2080; // p.57/59 (Table 42/47), 32-bit register, [15:0] is the sinc2 filter result — "if selected [i.e. LPFBYPEN=1, notch bypassed], the sinc2 filter output can be read via the SINC2DAT register" (p.57); ready flag is INTCFLAG0/1 bit2, NOT bit0
  static const uint16_t REG_INTCCLR     = 0x3004; // p.113 (Table 139), 32-bit, write-1-to-clear
  static const uint16_t REG_INTCSEL0    = 0x3008; // p.113/115, 32-bit, reset 0x2000 — bit0 (ADC result IRQ enable) and bit2 (Sinc2 result ready IRQ enable, p.115 INTSEL2) MUST be set for INTCFLAG0 bits 0/2 to ever assert
  static const uint16_t REG_INTCFLAG0   = 0x3010; // p.113/116-117 (Table 143), 32-bit — bit0 ADC result, bit2 Sinc2 filter result ready

  // ADC settling/timeout bound for triggerAndReadAdcCounts() — see note
  // there; not a datasheet number.
  static const unsigned long kAdcTimeoutUs = 5000;
  // ASSUMPTION (not a datasheet number): how many leading ADC results to
  // throw away after starting a conversion, so the sinc3 filter, PGA and
  // input mux have settled before the value that's kept. At the default
  // 160kHz sinc3 output rate (p.59) this costs ~20us.
  static const int kAdcDiscardResults = 3;
};
