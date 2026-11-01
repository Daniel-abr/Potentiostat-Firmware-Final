/*
 * sweep_params.h — the DPV waveform parameters, shared by main.cpp's sweep
 * engine and the onboard Parameters screen, plus their persistence in flash
 * (NVS) so edits made on the device survive a reboot.
 */
#pragma once
#include <Arduino.h>

struct SweepParams {
  String technique = "DPV";
  // Defaults match docs/Specifications for PalmSense4.txt (the Metrohm
  // Autolab DPV method this instrument is tuned to match for detecting
  // oseltamivir/efavirenz): 0.8V..1.3V, 5mV step, 25mV pulse amplitude,
  // 50ms modulation time, 0.5s interval time (~99 points, ~49.5s). With
  // kMaxV back at 1.30 (see SweepLimits below), endV=1.30 no longer gets
  // silently clamped by sanitizeSweepParams() — but these numbers predate
  // the sign-convention flip / VZERO0 change, so while they're no longer
  // out of range, whether 0.80V..1.30V in the CORRECTED E convention is
  // actually the electrode potential span this reference method intends
  // hasn't been independently re-verified.
  float startV = 0.80f;
  float endV   = 1.30f;
  float stepMv = 5.0f;
  float pulseAmpMv = 25.0f;
  float pulseWidthMs = 50.0f;
  float intervalMs = 500.0f;
  // One-time hold at startV before the first timed DPV step begins —
  // confirmed with the user after comparing a real electrochemical cell
  // against a PalmSens reference instrument: the jump from idle (E=0V) to
  // startV is the single largest potential step of the whole sweep (every
  // later step only moves by stepMv), so its double-layer charging
  // transient is much bigger/slower than any per-step dwell was ever
  // sized for — the first several points of a sweep otherwise read
  // noticeably off from a properly-equilibrated reference. 0 disables it
  // (matches the old behavior exactly). See AD5941::startSweep()'s
  // comment for the full mechanism. Raised from the original 3000ms to
  // 10000ms after a real-cell PalmSens comparison showed the start-of-sweep
  // gap was still present at 3s — this cell's double-layer/adsorption
  // transient needs longer than 3s to settle; retune per-solution via the
  // dashboard field if 10s still isn't enough (or is more than needed).
  float equilibrationMs = 10000.0f;
  String label = "sample";

  // ---- Fixed vs. auto-ranged RTIA (confirmed with the user) ----
  // false = use fixedRtiaCode directly (AD5941::useFixedRtia()), skipping
  // the pre-sweep probing pass entirely. true = the original
  // AD5941::autoRangeRtia() behavior. Re-enabled by default (reversing an
  // earlier "defaults to FIXED" decision) after a fixed-range sweep
  // overloaded immediately against an unexpectedly low-resistance load —
  // note autoRangeRtia() will often land on a range outside
  // kGainTable/kOffsetTable_nA's calibrated set (currently codes 2,5,6,7),
  // so a reading taken this way may be less accurate than one on a
  // hand-picked, calibrated fixedRtiaCode. Toggle in the dashboard's
  // "Auto-range RTIA" checkbox, or per-sweep via the WS start command.
  bool autoRange = true;
  // HSRTIACON[3:0] code (0=200ohm .. 7=160kohm, AD5941::kRtiaOhmsTable) used
  // when autoRange is false. 3 = 10kOhm, matching this rig's own dummy-
  // resistor testing so far — pick deliberately for your actual sample/cell.
  uint8_t fixedRtiaCode = 3;
};

// Limits the Parameters screen enforces (and loadSweepParams() re-applies to
// whatever comes back from flash).
namespace SweepLimits {
  // ad5941.h holds VZERO0 fixed at kVZero0Volts (now 1.7125V — 6-bit code
  // 44 exactly) and the LPDAC's VBIAS0 can only reach 0.2..2.4V (its true,
  // hard physical limits — going outside this range is what actually
  // clips/quantizes the pulse, per the bug described below). With the
  // corrected sign convention (E = VZERO0 - VBIAS0, see vbias0Code()):
  //   kMaxV=1.30 (raised back from 1.20, confirmed with the user, to match
  //     an external spec requirement calling for a 1.3V stop potential):
  //     worst case is base E at kMaxV WITH THE MOST POSITIVE pulse
  //     (kMaxPulseAmpMv, +100mV) — VBIAS0 = 1.7125-(1.30+0.100) = 0.3125V,
  //     still 0.1125V above the DAC's real 0.2V floor, so this NEVER
  //     actually clips, even at the largest allowed pulse. It only eats
  //     into an earlier, stricter self-imposed >=0.2V safety CUSHION
  //     (rather than the bare 0V-margin hard limit) — at the reference
  //     method's own 25mV pulse amplitude the margin is 0.1875V, and at
  //     0mV pulse it's 0.2125V (still clears the old >=0.2V target).
  //   kMinV=-0.35 (unchanged, not part of this request): worst case is
  //     base E at kMinV WITH THE MOST NEGATIVE pulse (kMinPulseAmpMv,
  //     -100mV) — VBIAS0 = 1.7125-(-0.35-0.100) = 2.1625V, 0.2375V below
  //     the 2.4V ceiling — still comfortably clear of the >=0.2V cushion,
  //     unlike kMaxV above.
  // Keep these in sync with AD5941::kVZero0Volts/kMinPulseAmpMv/
  // kMaxPulseAmpMv if any of them change — and recheck the kMaxV margin
  // arithmetic above specifically if kMaxPulseAmpMv is ever raised further,
  // since kMaxV no longer has the full >=0.2V cushion in reserve.
  //
  // CONFIRMED ON REAL HARDWARE (before the sign-convention fix) as the
  // likely cause of a sharp current deviation seen only in the last
  // handful of points of a sweep run at the OLD defaults/OLD sign
  // convention (startV=0.80, endV=1.30, pulseAmpMv=25): endV=kMaxV left
  // ZERO headroom for the pulse (not just a reduced cushion — the pulse
  // was clamped/truncated), so the last few steps' PULSE phase clamped
  // back down, shrinking the effective Ibefore/Iend difference toward
  // zero right at the sweep's top end — indistinguishable from a real
  // electrochemical background-current rise just from looking at the
  // trace. The current kMaxV=1.30 does NOT reintroduce this — see the
  // margin arithmetic above, which stays positive even at the max pulse.
  constexpr float kMinV = -0.35f;
  constexpr float kMaxV = 1.30f;
  constexpr float kMinStepMv = 1.0f,        kMaxStepMv = 50.0f;
  // Allows 0 and negative pulse amplitudes (item 4 — blank/open-circuit
  // tests at 0, -25, +25, +50mV need this) as well as the original
  // positive range. Symmetric with kMaxPulseAmpMv rather than picking a
  // smaller negative-side bound, for simplicity — see kMinV's comment
  // above for how this interacts with the DAC-rail margin.
  constexpr float kMinPulseAmpMv = -100.0f, kMaxPulseAmpMv = 100.0f;
  constexpr float kMinPulseWidthMs = 10.0f, kMaxPulseWidthMs = 500.0f;
  constexpr float kMinIntervalMs = 50.0f,   kMaxIntervalMs = 5000.0f;
  // 0 = no pre-sweep hold (matches the old, pre-equilibration-hold
  // behavior). 30s ceiling is just a sanity bound against accidentally
  // locking a sweep's start out for an absurd amount of time — not tied to
  // any particular electrochemistry.
  constexpr float kMinEquilibrationMs = 0.0f, kMaxEquilibrationMs = 30000.0f;
  // Pulse width has to leave the interval at least this much time at the
  // base level before the pre-pulse sample.
  constexpr float kMinBaseDwellMs = 10.0f;
  constexpr int kLabelMax = 10;
}

// Clamps every field into SweepLimits and enforces the cross-field rules
// (end above start by at least one step; pulse width shorter than the interval).
void sanitizeSweepParams(SweepParams &p);

// Number of points a sweep with these parameters produces — the same
// formula the sweep engine uses, so the on-screen estimate matches the run.
int sweepPointCount(const SweepParams &p);

// NVS persistence. load keeps p's existing values for any key not stored yet.
void loadSweepParams(SweepParams &p);
void saveSweepParams(const SweepParams &p);
