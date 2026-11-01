#include "ad5941.h"

// ===========================================================================
// SPI transport (p.104-106)
// ===========================================================================

void AD5941::spiSetAddr(uint16_t addr) {
  _spi.beginTransaction(_settings);
  digitalWrite(_cs, LOW);
  _spi.transfer(0x20); // SPICMD_SETADDR
  _spi.transfer((addr >> 8) & 0xFF);
  _spi.transfer(addr & 0xFF);
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();
}

void AD5941::writeReg(uint16_t addr, uint32_t value, uint8_t nBytes) {
  spiSetAddr(addr);
  _spi.beginTransaction(_settings);
  digitalWrite(_cs, LOW);
  _spi.transfer(0x2D); // SPICMD_WRITEREG
  for (int i = nBytes - 1; i >= 0; i--) {
    _spi.transfer((value >> (8 * i)) & 0xFF);
  }
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();
}

uint32_t AD5941::readReg(uint16_t addr, uint8_t nBytes) {
  spiSetAddr(addr);
  _spi.beginTransaction(_settings);
  digitalWrite(_cs, LOW);
  _spi.transfer(0x6D); // SPICMD_READREG
  _spi.transfer(0x00); // one dummy turnaround byte (p.104)
  uint32_t value = 0;
  for (uint8_t i = 0; i < nBytes; i++) {
    value = (value << 8) | _spi.transfer(0x00);
  }
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();
  return value;
}

// ===========================================================================
// Reset + power-up (p.25-29, p.32-33, p.38, p.41-42, p.52, p.62)
// ===========================================================================

// AFECON bits kept on for the whole time a sweep can run: DACBUFEN(21, dc
// DAC buffer — required whenever LPDACCON0 Bit5=1 routes VZERO0 to the HS
// chain, p.32), Reserved(19, "always set to 1", p.25), SINC2EN(16 — see
// correction below), TIAEN(11, HSTIA enable), ADCEN(7, ADC powered but
// idle). ADCCONVEN(8) is toggled on top of this per sample in
// triggerAndReadAdcCounts() (and held on continuously during a real sweep —
// see startSweep()).
//
// CORRECTION (found via hardware testing, not caught in the original
// datasheet read): SINC2EN's own bit description is explicit that Bit16=0
// "Disables sinc2 (50Hz/60Hz digital filter)" outright — not just the
// notch's extra math on top of an always-running sinc2 stage, as this
// driver originally (incorrectly) assumed when it left this bit at its
// reset default of 0 alongside ADCFILTERCON LPFBYPEN=1. With SINC2EN=0 the
// whole sinc2 block is powered down, so SINC2DAT and its ready flag
// (INTCFLAG0 bit2) never do anything — sampleSinc2Window() timed out with
// 0/18 samples on literally every single window during the first real
// sweep this was tested on. SINC2EN must be 1 for SINC2DAT to work AT ALL;
// LPFBYPEN (ADCFILTERCON, separately) is what then bypasses the notch's
// own extra computation on top of sinc2's output, which is what this
// driver actually wants (plain sinc2, no notch) — the two bits are
// independent, not redundant.
static const uint32_t AFECON_BASE = (1UL << 21) | (1UL << 19) | (1UL << 16) | (1UL << 11) | (1UL << 7);

// LPTIASW0 value for the Table 21 (p.38) "Voltammetry (Full Power Pulse
// Test) Using High Speed TIA" routing.
static const uint32_t LPTIASW0_PULSE_TEST = 0x0094;

// SWCON in group-control mode (SWSOURCESEL=0), p.77 Table 83: T9CON (bit17)
// closed, T10CON (bit18) open, TMUXCON[15:12]=0b0101 (T5 closed), and the
// N/P/D groups all "0xF = all switches open". Connects SE0 to the HSTIA
// inverting input through T9 (which also puts the HSRTIACON-selected RTIA
// in circuit).
static const uint32_t SWCON_HSTIA_SE0 = (1UL << 17) | (0x5UL << 12) | (0xFUL << 8) | (0xFUL << 4) | 0xFUL;

bool AD5941::reset() {
  digitalWrite(_reset, LOW);
  delay(10);
  digitalWrite(_reset, HIGH);
  delay(20); // >> the 22us wake-up time (tWK, p.17) the datasheet requires before SPI access

  uint16_t id = readReg16(REG_ADIID);
  if (id != 0x4144) return false; // p.28: ADIID is always 0x4144

  // Mandatory post-reset init sequence (Table 14, p.29) — the datasheet is
  // explicit that skipping/altering this leaves the device non-functional.
  // These are undocumented trim/calibration addresses taken verbatim; do
  // not "clean up" or reorder them.
  static const struct { uint16_t addr; uint16_t data; } kInitSeq[] = {
    {0x0908, 0x02C9}, {0x0C08, 0x206C}, {0x21F0, 0x0010}, {0x0410, 0x02C9},
    {0x0A28, 0x0009}, {0x238C, 0x0104}, {0x0A04, 0x4859}, {0x0A04, 0xF27B},
    {0x0A00, 0x8009}, {0x22F0, 0x0000},
  };
  for (auto &w : kInitSeq) writeReg16(w.addr, w.data);

  // Power up the blocks this driver uses. AFECON: DAC buffer + HSTIA + ADC
  // (see AFECON_BASE above), conversions left idle (ADCCONVEN=0) until a
  // sample is actually triggered.
  writeReg32(REG_AFECON, AFECON_BASE);

  // LPDAC: enable writes (RSTEN, bit0), pulse-test switch mode (DACMDE,
  // bit5=1 — required for the Table 21 "Voltammetry (Full Power Pulse
  // Test)" config below), 2.5V internal reference (REFSEL=0, default).
  writeReg32(REG_LPDACCON0, (1UL << 0) | (1UL << 5));

  // Electrode routing — literal values from Table 21, p.38, row
  // "Voltammetry (Full Power Pulse Test) Using High Speed TIA": VBIAS0
  // pulses CE0 via the potentiostat, VZERO0 feeds the HSTIA noninverting
  // input, HSTIA senses SE0.
  writeReg32(REG_LPDACSW0, 0x31);
  writeReg32(REG_LPTIASW0, LPTIASW0_PULSE_TEST);

  // Power up the potentiostat amp + LPTIA amp (clear PAPDEN/TIAPDEN, reset
  // default has both powered down). LPTIA's own gain resistor (TIAGAIN,
  // bits[9:5]) stays disconnected (0) since HSTIA — not LPTIA — does the
  // current-to-voltage conversion in this topology; both amps are still
  // powered up as a conservative choice (~2uA extra, p.15) since the
  // Table 21 switch routing runs signals through this block's switches
  // even though its own TIA output isn't the one being read. Verify on
  // hardware if you want to trim this for lower power once it's working.
  writeReg32(REG_LPTIACON0, 0x0);

  // HSTIA: VBIASSEL=01 selects VZERO0 as the noninverting input (p.45).
  // RTIA itself is now runtime-selectable per sweep (see autoRangeRtia()) —
  // this just sets the safe 1kOhm power-up default (matches autoRangeRtia()'s
  // own starting point) so sendStatus()/the dashboard show something sane
  // even before the first real sweep has run. CTIACON (parallel
  // stabilization cap, bits[12:5]) is now forced to kCtiaConBits (0x3F —
  // see that constant's comment for the bit5 discrepancy) on every
  // HSRTIACON write, confirmed with the user, rather than left at 0/none
  // as before.
  writeReg32(REG_HSTIACON, 0x1);
  _rtiaCode = 1;
  _correction = 1.0f;
  writeReg32(REG_HSRTIACON, kCtiaConBits | _rtiaCode);

  // HSTIA input switches. On power-up EVERY switch-matrix switch is open
  // (p.74; SWCON resets to 0x0000FFFF), which leaves the HSTIA disconnected
  // from SE0 with its RTIA out of circuit (HSRTIACON only takes effect with
  // T9 closed, p.52). Without this the current measurement path reads
  // nothing. T5 (via T9 and the fixed 100 ohm RLOAD_SE0, p.82) routes SE0
  // to the HSTIA inverting input. The D/N/P switches (high speed DAC
  // excitation amp) stay open, which also leaves the RCAL0/RCAL1 pins
  // (DR0/PR0/NR1/TR1) electrically disconnected.
  writeReg32(REG_SWCON, SWCON_HSTIA_SE0);

  // ADC: PGA gain 1.5x (bits[18:16]=001, the only factory-calibrated
  // setting), input mux = HSTIA+ / HSTIA- differential pair (p.57:
  // "Current and impedance measurement on the high speed TIA: MUXSEL_P =
  // positive high speed TIA output, MUXSEL_N = negative high speed TIA
  // input").
  writeReg32(REG_ADCCON, (0x1UL << 16) | (0x1UL << 8) | (0x1UL << 0));

  // Enable the "ADC result" AND "Sinc2 filter result ready" interrupt
  // sources so INTCFLAG0 bits 0 and 2 actually assert when each is ready
  // (p.115, Table 142: INTSEL0/INTSEL2 both reset to 0, i.e. neither flag
  // ever sets on its own). Bit0 backs the single-shot
  // triggerAndReadAdcCounts() path (autoRangeRtia()/selfTestDacAdcLoopback());
  // bit2 backs sampleSinc2Window()'s windowed averaging (poll()/
  // calibrateGain()). Keeps the reset-default bit13 (bootloader done) set.
  // This only drives the flag register we poll over SPI; the GPIO
  // interrupt output isn't used.
  writeReg32(REG_INTCSEL0, 0x00002005);

  // Bypass the 50Hz/60Hz notch filter's own extra computation (LPFBYPEN,
  // bit4) rather than use it: this driver instead reads the plain
  // sinc2-filter output (SINC2DAT) and averages it itself over a fixed
  // window per sample (see kSinc2WindowSamples/sampleSinc2Window() and
  // poll()) — chosen because the notch's own settling time (37ms, Table 3
  // p.31) barely fits a 50ms pulse with nothing left over, while bypassing
  // it drops that to 2.245ms, leaving a much larger margin for the same
  // averaging window. Every other field keeps the register's reset default
  // (0x301: SINC3OSR=5, SINC2OSR=178, i.e. bits[13:8]=0b000011) unchanged —
  // that's the ONLY sinc3/sinc2 combination the datasheet gives a
  // characterized settling time for (Table 3, p.31), so it's kept exactly
  // as-is rather than picked for a rounder sample count. Note this is
  // independent of AFECON Bit16 (SINC2EN, set in AFECON_BASE above) — that
  // bit gates the sinc2 stage itself (SINC2DAT doesn't function at all
  // without it, see AFECON_BASE's comment), while LPFBYPEN here only
  // bypasses the notch's additional filtering on top of sinc2's output.
  writeReg32(REG_ADCFILTERCON, 0x00000311);

  return true;
}

// ===========================================================================
// LPDAC bias-code math (p.30-31)
// ===========================================================================

uint16_t AD5941::vbias0Code(float requestedE) const {
  // VBIAS0 = VZERO0 - E — see the sign-convention comment on this
  // declaration in ad5941.h. A positive DPV pulse (E increasing) therefore
  // commands a SMALLER VBIAS0, not a larger one.
  float volts = kVZero0Volts - requestedE;
  volts = constrain(volts, kDacMinVolts, kDacMinVolts + 4095.0f * k12BitLsbVolts);
  long code = lroundf((volts - kDacMinVolts) / k12BitLsbVolts);
  return (uint16_t)constrain(code, 0L, 4095L);
}

uint16_t AD5941::vzero0Code() const {
  float volts = constrain(kVZero0Volts, kDacMinVolts, kDacMinVolts + 63.0f * k6BitLsbVolts);
  long code = lroundf((volts - kDacMinVolts) / k6BitLsbVolts);
  return (uint16_t)constrain(code, 0L, 63L);
}

uint16_t AD5941::writeLpdacCodes(uint16_t code12, uint16_t code6) {
  // Loading-effect correction (p.31, Eq.5-8): when the 12-bit code exceeds
  // 64x the 6-bit code, subtract one 12-bit LSB so the two DACs agree at
  // their nominal crossover point.
  if ((long)code12 > (long)code6 * 64) code12 -= 1;
  uint32_t value = ((uint32_t)code6 << 12) | code12;
  writeReg32(REG_LPDACDAT0, value);
  // LPDACDAT0 takes 10 clocks to settle at 16MHz sysclk (p.30) — a few
  // microseconds, negligible next to this driver's millisecond-scale
  // dwell times, so no extra wait is added here.
  return code12; // the FINAL (possibly -1 adjusted) code — see appliedE()
}

float AD5941::appliedE(uint16_t writtenCode12, uint16_t writtenCode6) const {
  // Datasheet Eq.5/6 (p.31), applied directly to whatever ended up in the
  // register — correct in both loading-effect cases because
  // writeLpdacCodes()'s -1 adjustment already pre-compensates code12 so a
  // plain per-Eq.5/6 readback matches the true output either way (p.31).
  float vbias0 = kDacMinVolts + (float)writtenCode12 * k12BitLsbVolts;
  float vzero0 = kDacMinVolts + (float)writtenCode6 * k6BitLsbVolts;
  return vzero0 - vbias0; // E = VZERO0 - VBIAS0
}

// ===========================================================================
// ADC trigger/read + current conversion (p.54-57)
// ===========================================================================

uint16_t AD5941::triggerAndReadAdcCounts(bool *readyOut) {
  writeReg32(REG_INTCCLR, 0x1); // drop any stale ADC-result flag first
  writeReg32(REG_AFECON, AFECON_BASE | (1UL << 8)); // ADCCONVEN=1

  // Wait for each ADC-result flag (INTCFLAG0 bit0, p.116 — requires
  // INTCSEL0 bit0, enabled in reset()), throwing away the first
  // kAdcDiscardResults results so filter/PGA/mux settling doesn't
  // contaminate the value kept. Each wait is bounded by kAdcTimeoutUs.
  // ASSUMPTION: the datasheet gives no cycle-accurate conversion latency
  // for this polled/non-sequencer usage, so 5ms is an engineering safety
  // margin, not a datasheet number — generous next to the ~6us/result at
  // the default 160kHz sinc3 output rate (p.59).
  bool ready = true;
  uint16_t counts = 0;
  for (int i = 0; i <= kAdcDiscardResults; i++) {
    bool got = false;
    unsigned long t0 = micros();
    while ((unsigned long)(micros() - t0) < kAdcTimeoutUs) {
      if (readReg32(REG_INTCFLAG0) & 0x1) { got = true; break; }
    }
    // Read ADCDAT even on a timeout so a stuck ADC shows a raw value
    // instead of a silent 0 that converts to a plausible-looking voltage.
    counts = (uint16_t)readReg32(REG_ADCDAT);
    writeReg32(REG_INTCCLR, 0x1);
    if (!got) { ready = false; break; }
  }

  writeReg32(REG_AFECON, AFECON_BASE); // ADCCONVEN=0, blocks stay powered
  if (readyOut) *readyOut = ready;
  return counts;
}

float AD5941::adcCountsToVolts(float counts) const {
  // p.56 Eq.13 (PGA=1.5x, factory-calibrated): VIN = (1.835V/PGA_G) *
  // (ADCDAT-0x8000)/2^15. No VBIAS_CAP addend — that only applies when
  // MUXSELN selects the VBIAS_CAP channel (code 0x8), which ours doesn't
  // (ours is the HSTIA- channel, code 0x1). counts is a float so a
  // fractional window MEAN converts directly (the formula is linear, so
  // this is equivalent to converting every sample then averaging the volts).
  float signedCounts = counts - 32768.0f;
  return (kAdcVref / kPgaGain) * (signedCounts / 32768.0f);
}

float AD5941::readHstiaCurrentNa() {
  bool ready = false;
  uint16_t counts = triggerAndReadAdcCounts(&ready);
  if (!ready) Serial.println("AD5941: ADC result flag timed out — this sample is unreliable");
  float volts = adcCountsToVolts(counts);
  // Latched raw/unlatched per-read; poll() ORs this into a per-step flag and
  // autoRangeRtia() reads it directly per probe point. Deliberately a RAW
  // primitive with no _correction applied — see autoRangeRtia()'s header
  // comment for why applying a range's own correction inside the function
  // that measures the data used to DERIVE that correction would be circular.
  _lastReadOverloaded = fabsf(volts) > (kOverloadFraction * kAdcInputWindowVolts);
  return (volts / rtiaOhmsForCode(_rtiaCode)) * 1.0e9f; // I = V/RTIA, in nA
}

AD5941::Sinc2Window AD5941::sampleSinc2Window(int numSamples) {
  Sinc2Window w;
  double sum = 0.0, sumSq = 0.0;
  writeReg32(REG_INTCCLR, 0x4); // drop any stale Sinc2-ready flag (bit2) first
  for (int i = 0; i < numSamples; i++) {
    bool got = false;
    unsigned long t0 = micros();
    // Same bounded busy-wait style/timeout as triggerAndReadAdcCounts()'s
    // ADC-result wait — the datasheet gives no cycle-accurate latency for
    // this polled usage either; at the ~1.1125ms sinc2 period this should
    // never come close to firing.
    while ((unsigned long)(micros() - t0) < kAdcTimeoutUs) {
      if (readReg32(REG_INTCFLAG0) & 0x4) { got = true; break; }
    }
    if (!got) { w.timedOut = true; break; }
    uint16_t counts = (uint16_t)readReg32(REG_SINC2DAT); // [15:0], p.59 Table 47
    writeReg32(REG_INTCCLR, 0x4);
    sum += counts;
    sumSq += (double)counts * (double)counts;
    w.sampleCount++;
  }
  if (w.sampleCount > 0) {
    w.meanCounts = (float)(sum / w.sampleCount);
    double variance = (sumSq / w.sampleCount) - ((double)w.meanCounts * (double)w.meanCounts);
    w.stdDevCounts = variance > 0.0 ? (float)sqrt(variance) : 0.0f;
  }
  return w;
}

float AD5941::readHstiaCurrentNaWindowed(Sinc2Window *outWindow) {
  // Ensure continuous conversion is on — idempotent if startSweep()/
  // calibrateGain() already set it; kept here too so this function is
  // correct standalone regardless of caller.
  writeReg32(REG_AFECON, AFECON_BASE | (1UL << 8)); // ADCCONVEN=1
  Sinc2Window w = sampleSinc2Window(kSinc2WindowSamples);
  if (w.timedOut) {
    Serial.printf("AD5941: Sinc2 result flag timed out mid-window (got %d/%d samples) — this reading is unreliable\n",
                  w.sampleCount, kSinc2WindowSamples);
  }
  if (outWindow) *outWindow = w;
  float volts = adcCountsToVolts(w.meanCounts);
  // Raw — no calibration applied, same reasoning as readHstiaCurrentNa():
  // applying a range's own correction inside the function that measures
  // the data used to derive/apply it would be circular for
  // autoRangeRtia()/calibrateGain()/calPoint(), which all call this
  // indirectly-equivalent raw path. poll() applies applyCalibration()
  // itself, once, after this returns.
  _lastReadOverloaded = fabsf(volts) > (kOverloadFraction * kAdcInputWindowVolts);
  if (_lastReadOverloaded) {
    // Item B — always-on (not gated by setDebugWindows()) since an
    // overload abort is rare and important enough to want the raw numbers
    // every time, to tell a genuine saturation apart from a charging-
    // transient false-positive at the very first point of a sweep.
    Serial.printf("AD5941: OVERLOAD - step=%d volts=%.4f meanCounts=%.1f rtiaCode=%d\n",
                  _step, volts, w.meanCounts, _rtiaCode);
  }
  return (volts / rtiaOhmsForCode(_rtiaCode)) * 1.0e9f;
}

float AD5941::applyCalibration(float rawNa, uint8_t code) const {
  // See kGainTable/kOffsetTable_nA's comment (ad5941.h) for the full
  // rationale — this replaces the previous either/or logic against a
  // gain-only table; the ratiometric _correction is no longer consulted.
  uint8_t c = (uint8_t)constrain(code, kRtiaCodeMin, kRtiaCodeMax);
  return kGainTable[c] * rawNa + kOffsetTable_nA[c];
}

void AD5941::setRtiaCode(uint8_t code) {
  code = (uint8_t)constrain(code, kRtiaCodeMin, kRtiaCodeMax);
  // TIASW6CON (bit4) is preserved (read-modify-write); CTIACON (bits[12:5])
  // is forced to kCtiaConBits on every write (confirmed with the user, item
  // 3) rather than preserved, so every RTIA range change also applies the
  // stabilization cap consistently — reset() does the same for its own
  // direct HSRTIACON write.
  uint32_t cur = readReg32(REG_HSRTIACON);
  uint32_t tiasw6 = cur & (1UL << 4);
  writeReg32(REG_HSRTIACON, kCtiaConBits | tiasw6 | code);
  _rtiaCode = code;
}

void AD5941::useFixedRtia(uint8_t code) {
  setRtiaCode(code);
  delay(kRtiaSwitchSettleMs);
  // No probing pass ran, so there is nothing to compute a ratiometric
  // self-check factor FROM — irrelevant anyway now (see applyCalibration(),
  // which only consults kGainTable/kOffsetTable_nA), kept at neutral (1.0)
  // just so the diagnostic AfeRangeResult::correction field reads sanely.
  _correction = 1.0f;
}

void AD5941::holdStaticE(float requestedE) {
  writeLpdacCodes(vbias0Code(requestedE), vzero0Code());
}

// ===========================================================================
// DPV staircase+pulse sweep state machine
// ===========================================================================

bool AD5941::configureSweep(float startV, float endV, float stepV, float pulseAmpV,
                             float pulseWidthMs, float intervalMs, float equilibrationMs) {
  _equilibrationMs = (unsigned long)max(0.0f, equilibrationMs);
  _startV = startV;
  _endV = endV;
  // stepV arrives as a plain positive magnitude (main.cpp passes
  // p.stepMv/1000.0f, always >=0 per SweepLimits) — the actual scan
  // DIRECTION is derived here from whichever of startV/endV is larger, so
  // the caller doesn't need to pre-negate anything for a descending sweep
  // (e.g. startV=0.6, endV=-0.2). poll()'s per-step base level is just
  // _startV + _stepV*_step, so a negative _stepV walks the staircase
  // downward with no other change needed anywhere else in the state
  // machine.
  _stepV = (endV >= startV) ? stepV : -stepV;
  _pulseAmpV = pulseAmpV;
  // Fixed integer LSB delta for the pulse (item 3d, confirmed with the
  // user): e.g. 25mV / 0.537240537mV per LSB = 46.53 -> rounds to 47 LSB
  // = 25.2503mV actually applied, EVERY step, rather than each step
  // independently rounding its own absolute pulse target. Signed, so a
  // negative pulseAmpV (blank-test pulses, item 4) works the same way.
  _pulseDeltaLsb = (int)lroundf(pulseAmpV / k12BitLsbVolts);
  _pulseWidthMs = (unsigned long)max(1.0f, pulseWidthMs);
  // Time spent at the base (pre-pulse) level each step, before sampling
  // Ibefore — the remainder of the step's total time budget after the
  // pulse itself. Matches the reference method's "interval time" (total
  // per-step budget, 0.5s) vs "modulation time" (pulse duration, 0.05s).
  unsigned long interval = (unsigned long)max((float)_pulseWidthMs + 1.0f, intervalMs);
  _baseSettleMs = interval - _pulseWidthMs;

  // fabsf: the point COUNT only depends on the span's magnitude, not which
  // direction it's being walked in — direction is entirely carried by
  // _stepV's sign above.
  _totalSteps = stepV > 0 ? (int)(fabsf(endV - startV) / stepV) : 0;
  if (_totalSteps <= 0) _totalSteps = 1;

  _step = 0;
  _phase = Phase::IDLE;

  // The Ibefore/Iend averaging window has to fit entirely inside both the
  // pulse width and the base dwell, with kSettlingMarginMs of settling time
  // left over before the window starts (see that constant's comment).
  // SweepLimits (sweep_params.h) allows pulse widths/base dwells as short
  // as 10ms — well under this floor — so this is a real, reachable
  // failure. Per explicit instruction: report it and refuse to configure
  // rather than silently truncating the window or changing the requested
  // timing.
  float minMs = kSinc2WindowMs + kSettlingMarginMs;
  if ((float)_pulseWidthMs < minMs || (float)_baseSettleMs < minMs) {
    Serial.printf("AD5941: sweep timing too tight for the ADC averaging window — need pulse "
                  "width AND (interval - pulse width) both >= %.1fms; got pulse=%lums, "
                  "base dwell=%lums. Increase pulse width and/or interval time.\n",
                  minMs, _pulseWidthMs, _baseSettleMs);
    return false;
  }
  return true;
}

void AD5941::startSweep() {
  _step = 0;
  _baseCode12 = writeLpdacCodes(vbias0Code(_startV), vzero0Code());
  // Leave the ADC converting continuously for the whole sweep (rather than
  // toggling ADCCONVEN per sample, as the single-shot path does) — the
  // windowed averaging in poll() only ever reads two ~20ms slices out of
  // each step, but restarting conversion at the start of every slice would
  // reintroduce a settling transient the datasheet gives no number for, on
  // every window instead of once here. stopSweep() turns it back off.
  writeReg32(REG_AFECON, AFECON_BASE | (1UL << 8)); // ADCCONVEN=1
  // EQUILIBRATE first, not straight into BASE_SETTLE (confirmed with the
  // user, after comparing against a PalmSens reference on a real
  // electrochemical cell): the jump from the idle bias (E=0V, stopSweep()'s
  // safe-idle point) to _startV is the single LARGEST potential step of the
  // whole sweep — every step after this one only moves by _stepV (a few
  // mV) — so the double-layer charging transient it provokes is much
  // bigger and slower than what the per-step base dwell was ever sized
  // for. Real-hardware comparison showed exactly this: the first several
  // points of a sweep read far more negative than the reference instrument
  // while the rest of the scan tracked it closely. _equilibrationMs (set
  // by configureSweep() from SweepParams::equilibrationMs, 10000ms by
  // default — raised from an initial 3000ms after a follow-up real-cell
  // comparison showed the gap was still present at 3s; 0 disables it
  // entirely — SweepLimits::kMinEquilibrationMs)
  // holds at _startV before the first timed step's own dwell begins,
  // giving the cell a dedicated settling period separate from (and
  // typically much longer than) a single step's dwell.
  _phase = (_equilibrationMs > 0) ? Phase::EQUILIBRATE : Phase::BASE_SETTLE;
  _phaseStartMs = millis();
}

void AD5941::stopSweep() {
  _phase = Phase::IDLE;
  writeReg32(REG_AFECON, AFECON_BASE); // ADCCONVEN=0, blocks stay powered (matches idle default)
  writeLpdacCodes(vbias0Code(0.0f), vzero0Code()); // back to ~0V relative bias, safe idle
}

bool AD5941::poll(float &outV, float &outDeltaINa) {
  if (_phase == Phase::IDLE || _step >= _totalSteps) return false;

  unsigned long now = millis();

  // One-time pre-sweep hold at _startV — see startSweep()'s comment for
  // why this exists separately from BASE_SETTLE's own per-step dwell.
  // Non-blocking like every other phase: just keep returning false until
  // the hold elapses, then fall into the normal step 0 BASE_SETTLE dwell
  // with a fresh timestamp (not the one startSweep() set — otherwise the
  // equilibration hold would double-count as part of step 0's own dwell).
  if (_phase == Phase::EQUILIBRATE) {
    if (now - _phaseStartMs < _equilibrationMs) return false;
    _phase = Phase::BASE_SETTLE;
    _phaseStartMs = now;
    return false;
  }

  float baseV = _startV + _stepV * _step;

  // Lead time before each phase's nominal end at which the fixed-count
  // averaging window must START so its LAST sample lands right at the edge
  // (base->pulse transition for Ibefore, end-of-pulse for Iend) instead of
  // averaging across it. Ceil'd to a whole millisecond for this
  // millis()-based comparison, same as every other phase transition in
  // this driver — this is deliberately software-timed and can jitter by up
  // to one loop() tick, which is why kSettlingMarginMs (baked into
  // configureSweep()'s fit check) is several times the datasheet's actual
  // 2.245ms settling figure rather than equal to it. configureSweep()
  // already guarantees both _baseSettleMs and _pulseWidthMs are at least
  // kSinc2WindowMs + kSettlingMarginMs, so the subtractions below can't
  // underflow.
  static const unsigned long kWindowLeadMs = (unsigned long)ceil(kSinc2WindowMs); // 21ms

  if (_phase == Phase::BASE_SETTLE) {
    if (now - _phaseStartMs < _baseSettleMs - kWindowLeadMs) return false;
    _stepOverloaded = false;
    Sinc2Window w;
    // Blocks for ~kSinc2WindowMs (~20ms) — see readHstiaCurrentNaWindowed().
    _iBeforeNa = applyCalibration(readHstiaCurrentNaWindowed(&w), _rtiaCode);
    _stepOverloaded |= _lastReadOverloaded;
    // Pulse code = base code - pulseDeltaLsb, a FIXED integer delta from
    // _baseCode12 (item 3d) — NOT vbias0Code() applied to the absolute
    // pulse target (baseV+pulseAmpV), which would independently round
    // each step's pulse and could drift by +/-1 LSB step to step.
    // Subtracting because VBIAS0=VZERO0-E and a positive pulse increases
    // E. pulseV below is only the IDEAL target, kept for the log line.
    float pulseV = baseV + _pulseAmpV;
    long pulseCodeSigned = (long)_baseCode12 - (long)_pulseDeltaLsb;
    uint16_t pulseCode = (uint16_t)constrain(pulseCodeSigned, 0L, 4095L);
    writeLpdacCodes(pulseCode, vzero0Code());
    // Timestamp AFTER the blocking capture above, not the `now` from the
    // top of this call — the capture itself takes ~20ms, so using the
    // pre-capture timestamp here would make PULSE_SETTLE's own elapsed-time
    // tracking think that ~20ms had already passed the moment the pulse
    // edge was written, pulling the Iend window ~20ms too early and
    // wiping out the settling margin the whole design relies on.
    unsigned long pulseEdgeMs = millis();
    // Ground-truth timing/value for correlating against a scope trace —
    // there was previously no per-step log at all during the real sweep
    // (only the pre-sweep probe logged anything), so "are pulses actually
    // being written" could only be inferred, not checked directly.
    // pulseCode is what actually got sent over SPI, in raw DAC counts —
    // compare it against kDacMinVolts+pulseCode*k12BitLsbVolts by hand if
    // you want to rule out the constrain() clamp above (pulseCodeSigned ->
    // pulseCode) silently capping the pulse at the DAC's 0.2V floor for E
    // values near the top of the sweep range (VBIAS0 = VZERO0 - E now
    // DECREASES as E rises — see the sign-convention comment on
    // vbias0Code()).
    Serial.printf("  step %d @t=%lums: base=%.3fV -> pulse target=%.3fV (DAC code %u) Ibefore=%.1fnA\n",
                  _step, pulseEdgeMs, kVZero0Volts - baseV, kVZero0Volts - pulseV, pulseCode, _iBeforeNa);
    if (_debugWindows) {
      Serial.printf("    Ibefore window: n=%d/%d mean_raw=%.1f sd_raw=%.2f%s\n",
                    w.sampleCount, kSinc2WindowSamples, w.meanCounts, w.stdDevCounts,
                    w.timedOut ? " (TIMED OUT)" : "");
    }
    _phase = Phase::PULSE_SETTLE;
    _phaseStartMs = pulseEdgeMs;
    return false;
  }

  // Phase::PULSE_SETTLE
  if (now - _phaseStartMs < _pulseWidthMs - kWindowLeadMs) return false;
  Sinc2Window w;
  float iEndNa = applyCalibration(readHstiaCurrentNaWindowed(&w), _rtiaCode);
  _lastIEndNa = iEndNa; // latched for lastIEndNa() — see main.cpp's CSV/WS logging
  _stepOverloaded |= _lastReadOverloaded;
  _lastSampleOverloaded = _stepOverloaded;
  unsigned long stepEndMs = millis(); // after the blocking capture — see the comment above
  Serial.printf("  step %d @t=%lums: pulse end, Iend=%.1fnA deltaI=%.1fnA\n",
                _step, stepEndMs, iEndNa, iEndNa - _iBeforeNa);
  if (_debugWindows) {
    Serial.printf("    Iend window: n=%d/%d mean_raw=%.1f sd_raw=%.2f%s\n",
                  w.sampleCount, kSinc2WindowSamples, w.meanCounts, w.stdDevCounts,
                  w.timedOut ? " (TIMED OUT)" : "");
  }

  // The actually-applied E for THIS step's base level, computed from the
  // codes really written (_baseCode12, set either by startSweep() for step 0
  // or by this same block on the previous iteration for step>0) rather than
  // the ideal/requested baseV — captures DAC quantization and any
  // constrain()-clamping near the DAC rails automatically (item 2).
  outV = appliedE(_baseCode12, vzero0Code());
  outDeltaINa = iEndNa - _iBeforeNa;

  _step++;
  if (_step < _totalSteps) {
    _baseCode12 = writeLpdacCodes(vbias0Code(_startV + _stepV * _step), vzero0Code());
    _phase = Phase::BASE_SETTLE;
    _phaseStartMs = stepEndMs;
  } else {
    _phase = Phase::IDLE;
  }
  return true;
}

// ===========================================================================
// Bench self-test (no electrode/cell/jumper required)
// ===========================================================================

bool AD5941::selfTestDacAdcLoopback() {
  // The ADCCON value reset()/normal sweep operation expects — HSTIA+/HSTIA-
  // differential pair, PGA 1.5x (see reset()) — saved here so it can be
  // restored after the test temporarily points the mux elsewhere.
  static const uint32_t kAdcconNormal = (0x1UL << 16) | (0x1UL << 8) | (0x1UL << 0);

  Serial.println("AD5941 self-test: LPDAC -> ADC loopback (no electrode required)");

  // Register readbacks — proves SPI *writes* land, not just that the chip
  // ID read works. Informational only (doesn't affect PASS/FAIL): a
  // MISMATCH here points at the write path or a register that behaves
  // differently than assumed.
  auto show = [&](const char *name, uint16_t addr, uint32_t expected) {
    uint32_t v = readReg32(addr);
    Serial.printf("  %-9s = 0x%08X (expected 0x%08X)%s\n", name, (unsigned)v, (unsigned)expected,
                  v == expected ? "" : "  <-- MISMATCH");
  };
  show("AFECON", REG_AFECON, AFECON_BASE);
  show("LPDACCON0", REG_LPDACCON0, (1UL << 0) | (1UL << 5));
  show("INTCSEL0", REG_INTCSEL0, 0x00002001);
  show("ADCCON", REG_ADCCON, kAdcconNormal);
  show("SWCON", REG_SWCON, SWCON_HSTIA_SE0);

  // Points chosen to stay inside the ADC's +/-0.9V differential input window
  // at PGA 1.5x (p.37) relative to VBIAS_CAP (1.11V): -0.61V .. +0.79V.
  static const float kTestVolts[] = {0.5f, 1.1f, 1.5f, 1.9f};
  bool allOk = true;

  // The Table 21 pulse-test routing (LPTIASW0=0x0094) leaves the potentiostat
  // open loop and the VBIAS0 pin undriven (SW3/SW12 open — the capacitors
  // are deliberately disconnected), so neither CE0 nor the VBIAS0-pin ADC
  // tap follows the DAC by itself. Add SW10 (LPTIASW0 bit 10, p.36/p.38:
  // feeds the potentiostat's output back to its inverting input) to make
  // it a unity-gain follower: CE0 then tracks VBIAS0, which reaches the
  // potentiostat's noninverting input directly through SW4 (in 0x31) —
  // the same DAC -> potentiostat path a real sweep uses, with no external
  // capacitor settling in the way. Restored below.
  //
  // HARDWARE NOTE: SW10 also ties RE0 to CE0 internally, so the
  // BUF_VREF1V8 -> RE0 jumper MUST be off for this test.
  writeReg32(REG_LPTIASW0, LPTIASW0_PULSE_TEST | (1UL << 10));

  for (float target : kTestVolts) {
    // Command VBIAS0 to target; hold VZERO0 at its normal fixed point so
    // this doesn't disturb anything else the driver assumes. vbias0Code()
    // now computes VBIAS0 = VZERO0 - E (sign-convention fix), so the E that
    // makes VBIAS0 land on target is (VZERO0 - target) — the OLD order
    // (target - kVZero0Volts) was for the pre-fix "VZERO0 + E" formula and
    // would now compute a completely different, wrong VBIAS0.
    writeLpdacCodes(vbias0Code(kVZero0Volts - target), vzero0Code());
    delay(10); // op-amp + CE0 settling; well beyond the 10-clock LPDAC update (p.30)

    // ADCCON: PGA 1.5x, MUXSELP=CE0 pin (0b011001, p.63), MUXSELN=VBIAS_CAP
    // (0b01000, the datasheet's general "voltage measurement" pairing,
    // p.56-57).
    writeReg32(REG_ADCCON, (0x1UL << 16) | (0x8UL << 8) | (0x19UL << 0));
    bool ready = false;
    uint16_t counts = triggerAndReadAdcCounts(&ready);
    int32_t signedCounts = (int32_t)counts - 0x8000;
    float measured = (kAdcVref / kPgaGain) * ((float)signedCounts / 32768.0f) + kVBiasCapVolts;

    float errorV = measured - target;
    bool ok = ready && fabsf(errorV) < kSelfTestToleranceV;
    allOk = allOk && ok;
    Serial.printf("  CE0 (follower) target=%.3fV measured=%.3fV error=%+.3fV raw=0x%04X%s %s\n",
                  target, measured, errorV, (unsigned)counts,
                  ready ? "" : " (ADC ready flag timed out)", ok ? "PASS" : "FAIL");
  }

  // Restore the normal HSTIA-facing ADC config, pulse-test routing and idle bias.
  writeReg32(REG_ADCCON, kAdcconNormal);
  writeReg32(REG_LPTIASW0, LPTIASW0_PULSE_TEST);
  writeLpdacCodes(vbias0Code(0.0f), vzero0Code());

  Serial.println(allOk ? "AD5941 self-test: PASS" : "AD5941 self-test: FAIL");
  return allOk;
}

// ===========================================================================
// Safety-first auto-ranging probe (this project's own design — see the
// header comment on autoRangeRtia() for the full rationale/algorithm)
// ===========================================================================

AfeRangeResult AD5941::autoRangeRtia(const float *probeVolts, int numProbeVolts,
                                      bool (*stopRequested)()) {
  AfeRangeResult result;

  setRtiaCode(1); // always start at 1kOhm regardless of whatever code was active before
  delay(kRtiaSwitchSettleMs);

  // Logs each individual probe sample (not just the per-range peak) so a
  // suspiciously large FIRST reading right after a bias jump — e.g. a
  // charging-current transient into stray capacitance that kProbeBiasSettleMs
  // didn't wait out — is visible directly, rather than only seeing the
  // ranging decision it produced.
  auto probeOnce = [&]() -> float {
    float peak = 0.0f;
    for (int i = 0; i < numProbeVolts; i++) {
      if (stopRequested && stopRequested()) return peak; // item A — abort between steps
      writeLpdacCodes(vbias0Code(probeVolts[i]), vzero0Code());
      delay(kProbeBiasSettleMs);
      float iNa = fabsf(readHstiaCurrentNa()); // raw — no _correction
      Serial.printf("  probe V=%+.3f -> %.1f nA%s\n", probeVolts[i], iNa,
                    iNa > peak ? " (new peak)" : "");
      if (iNa > peak) peak = iNa;
    }
    return peak;
  };

  Serial.printf("Auto-range: probing at 1000 ohm\n");
  float peakNa = probeOnce();
  float correction = 1.0f;

  while (!(stopRequested && stopRequested())) {
    float fullScaleNa = (kAdcInputWindowVolts / rtiaOhmsForCode(_rtiaCode)) * 1.0e9f;
    float pct = fullScaleNa > 0.0f ? (peakNa / fullScaleNa) * 100.0f : 100.0f;
    bool atMax = (_rtiaCode >= kRtiaCodeMax);
    Serial.printf("Auto-range: %.0f ohm peak=%.1f nA (%.1f%% of %.0f nA full scale)%s\n",
                  rtiaOhmsForCode(_rtiaCode), peakNa, pct, fullScaleNa,
                  pct >= kAutoRangeStepUpThresholdPct ? " -> stop (in range)" : "");

    if (pct >= kAutoRangeStepUpThresholdPct || atMax) {
      result.atMaxRange = atMax && pct < kAutoRangeStepUpThresholdPct;
      break;
    }

    float prevPeakNa = peakNa;
    setRtiaCode(_rtiaCode + 1);
    delay(kRtiaSwitchSettleMs);
    Serial.printf("Auto-range: probing at %.0f ohm\n", rtiaOhmsForCode(_rtiaCode));
    peakNa = probeOnce();
    // Ratiometric self-check: the true current at these same probe
    // potentials should read the same on both ranges. Floor the
    // denominator so a near-zero re-probe can't blow the factor up.
    correction *= prevPeakNa / max(peakNa, kEpsilonNa);
  }

  if (stopRequested && stopRequested()) {
    Serial.println("Auto-range: aborted (stop requested mid-probe)");
    result.aborted = true;
  }

  _correction = correction;
  writeLpdacCodes(vbias0Code(0.0f), vzero0Code()); // idle, same as stopSweep()

  result.rtiaCode = _rtiaCode;
  result.rtiaOhms = rtiaOhmsForCode(_rtiaCode);
  result.correction = correction;
  result.peakProbeNa = peakNa * correction;
  return result;
}

// ===========================================================================
// Gain calibration against a known resistor (this project's own design —
// see calibrateGain()'s header comment for the full rationale)
// ===========================================================================

void AD5941::calibrateGain(float knownOhms, uint8_t rtiaCode) {
  // Two probe potentials, deliberately kept close to the fixed VZERO0
  // reference point (0V and +0.2V relative — the user's own example
  // spacing) rather than sweeping near either DAC rail, so this stays a
  // small, well-behaved, one-off bench measurement. 0.2V was picked, not
  // requested as a parameter, to keep the calibrate <ohms> <rtiaCode>
  // command signature at two arguments.
  static constexpr float kCalibrationBaseV = 0.0f;
  static constexpr float kCalibrationDeltaV = 0.2f;

  setRtiaCode(rtiaCode);
  delay(kRtiaSwitchSettleMs);
  writeReg32(REG_AFECON, AFECON_BASE | (1UL << 8)); // ADCCONVEN=1 for the duration

  writeLpdacCodes(vbias0Code(kCalibrationBaseV), vzero0Code());
  delay(kProbeBiasSettleMs);
  Sinc2Window wA = sampleSinc2Window(kSinc2WindowSamples);
  float voltsA = adcCountsToVolts(wA.meanCounts);
  float iA_nA = (voltsA / rtiaOhmsForCode(rtiaCode)) * 1.0e9f;

  writeLpdacCodes(vbias0Code(kCalibrationBaseV + kCalibrationDeltaV), vzero0Code());
  delay(kProbeBiasSettleMs);
  Sinc2Window wB = sampleSinc2Window(kSinc2WindowSamples);
  float voltsB = adcCountsToVolts(wB.meanCounts);
  float iB_nA = (voltsB / rtiaOhmsForCode(rtiaCode)) * 1.0e9f;

  writeReg32(REG_AFECON, AFECON_BASE); // ADCCONVEN=0, idle
  writeLpdacCodes(vbias0Code(0.0f), vzero0Code());

  // Current DIFFERENCE against 1/R, not a single-point V/R reading — any
  // DAC/VZERO0 or ADC offset is approximately constant across these two
  // nearby bias points and cancels out of the difference, leaving only the
  // gain error this factor is meant to capture (confirmed with the user).
  float measuredDeltaINa = iB_nA - iA_nA;
  float expectedDeltaINa = (kCalibrationDeltaV / knownOhms) * 1.0e9f;
  float factor = (measuredDeltaINa != 0.0f) ? (expectedDeltaINa / measuredDeltaINa) : 0.0f;
  float fullScaleNa = (kAdcInputWindowVolts / rtiaOhmsForCode(rtiaCode)) * 1.0e9f;

  Serial.printf("Calibrate: RTIA=%.0f ohm (code %d), known=%.1f ohm, deltaV=%.3fV\n",
                rtiaOhmsForCode(rtiaCode), rtiaCode, knownOhms, kCalibrationDeltaV);
  Serial.printf("  point A: V=%.4f mean_raw=%.1f (n=%d/%d, sd=%.2f) -> %.2f nA\n",
                kCalibrationBaseV, wA.meanCounts, wA.sampleCount, kSinc2WindowSamples, wA.stdDevCounts, iA_nA);
  Serial.printf("  point B: V=%.4f mean_raw=%.1f (n=%d/%d, sd=%.2f) -> %.2f nA\n",
                kCalibrationBaseV + kCalibrationDeltaV, wB.meanCounts, wB.sampleCount, kSinc2WindowSamples, wB.stdDevCounts, iB_nA);
  Serial.printf("  measured deltaI=%.2f nA, expected deltaI=%.2f nA (%.1f%% of %.0f nA full scale)\n",
                measuredDeltaINa, expectedDeltaINa, fullScaleNa > 0.0f ? fabsf(measuredDeltaINa) / fullScaleNa * 100.0f : 0.0f,
                fullScaleNa);
  Serial.printf("  -> gain factor for kGainTable[%d] = %.4f (offset unchanged — this method doesn't "
                "estimate one; see calPoint() if you also want kOffsetTable_nA[%d])\n", rtiaCode, factor, rtiaCode);
  Serial.println("  (printed only — hand-edit kGainTable in ad5941.h to apply it, after checking this looks sane / repeats)");
  if (fabsf(measuredDeltaINa) > kOverloadFraction * fullScaleNa) {
    Serial.println("  WARNING: measured deltaI exceeds the overload margin for this range — "
                    "pick a larger known resistor or a lower RTIA code and recalibrate.");
  }
}

// ===========================================================================
// Raw single-point diagnostic (this project's own design — see
// calPoint()'s header comment for the full rationale)
// ===========================================================================

void AD5941::calPoint(uint8_t rtiaCode, float requestedE) {
  useFixedRtia(rtiaCode); // fixes RTIA, forces kCtiaConBits (via setRtiaCode()), auto-ranging untouched
  holdStaticE(requestedE); // corrected sign convention (VBIAS0 = VZERO0 - E)
  delay(3000); // 3s settle, per spec

  writeReg32(REG_AFECON, AFECON_BASE | (1UL << 8)); // ADCCONVEN=1 for the capture
  // ~2s of sinc2 samples: round(2000ms / kSinc2SamplePeriodMs) = 1798
  // samples. Reuses sampleSinc2Window() (the same primitive poll()'s
  // 20ms windows use) rather than a new wall-clock-bounded capture path —
  // unlike poll()'s Ibefore/Iend windows, this has no edge-alignment
  // requirement (nothing is transitioning during this hold), so a large
  // fixed count that closely approximates 2s is simplest. Blocking for
  // the whole ~2s is fine here — this is an explicit bench command, not
  // part of the timing-critical DPV step loop.
  static const int kCalPointSamples = (int)lroundf(2000.0f / kSinc2SamplePeriodMs); // 1798
  Sinc2Window w = sampleSinc2Window(kCalPointSamples);
  writeReg32(REG_AFECON, AFECON_BASE); // ADCCONVEN=0 — the HELD BIAS is unaffected, still applied

  if (w.timedOut) {
    Serial.printf("AD5941: calpoint Sinc2 flag timed out mid-capture (got %d/%d samples) — unreliable\n",
                  w.sampleCount, kCalPointSamples);
  }

  float volts = adcCountsToVolts(w.meanCounts);
  float rawNa = (volts / rtiaOhmsForCode(rtiaCode)) * 1.0e9f;
  // Standard deviation in nA: the counts->volts->nA chain is linear, so
  // SD scales by the same slope as the mean did — get that slope directly
  // from adcCountsToVolts() (volts-per-count) rather than re-deriving the
  // Vref/PGA/32768 constants a second time.
  float voltsPerCount = adcCountsToVolts(1.0f) - adcCountsToVolts(0.0f);
  float stdDevNa = w.stdDevCounts * voltsPerCount / rtiaOhmsForCode(rtiaCode) * 1.0e9f;

  Serial.printf("Calpoint: RTIA=%.0f ohm (code %d), E=%.4fV, n=%d/%d%s\n",
                rtiaOhmsForCode(rtiaCode), rtiaCode, requestedE, w.sampleCount, kCalPointSamples,
                w.timedOut ? " (TIMED OUT)" : "");
  Serial.printf("  RAW mean=%.2f nA, RAW stddev=%.2f nA (mean_raw_code=%.1f, sd_raw_code=%.2f)\n",
                rawNa, stdDevNa, w.meanCounts, w.stdDevCounts);
  // Flags when the HSTIA output has left the ADC's recommended linear
  // window (kAdcInputWindowVolts, +/-0.9V — Table 1's own "<+/-900mV"
  // guidance) — found the hard way: a calpoint run at E=0.5V against a
  // ~94kOhm resistor landed at 0.928V here, and the resulting "corrected"
  // reading was off by ~6.6% even though the SAME calibration checked out
  // to within 0.4% at E=0.2V on the same resistor (0.397V, safely inside
  // the window). The gain/offset fit is characterized from calibration
  // points taken well inside this window (+/-0.5V against resistors sized
  // to stay under it) and has no reason to stay linear once a reading
  // exceeds it — a smaller E, a smaller known resistor, or a lower RTIA
  // code all reduce this same output voltage.
  if (fabsf(volts) > kAdcInputWindowVolts) {
    Serial.printf("  WARNING: HSTIA output %.3fV exceeds the ADC's recommended +/-%.1fV linear "
                  "window (Table 1) — this reading may be nonlinear/compressed, and the gain/offset "
                  "calibration (characterized inside this window) may not apply accurately here. "
                  "Try a smaller E, a smaller known resistor, or a lower RTIA code.\n",
                  volts, kAdcInputWindowVolts);
  }
  // Display-only use of applyCalibration() — calPoint()'s own returned/
  // logged measurement stays raw (rawNa above), so this doesn't reintroduce
  // the circularity that keeps this function raw in the first place; it
  // just shows what the CURRENT kGainTable/kOffsetTable_nA would produce,
  // for a quick sanity check against an expected value (e.g. E/knownOhms).
  Serial.printf("  Corrected (kGainTable[%d]*raw+kOffsetTable_nA[%d]) = %.2f nA\n",
                rtiaCode, rtiaCode, applyCalibration(rawNa, rtiaCode));
  Serial.println("  E is still held — probe with a DMM now, or run \"hold 0\" to release.");
}
