#include "sweep_params.h"
#include <Preferences.h>

static const char *kNamespace = "openstat";

void sanitizeSweepParams(SweepParams &p) {
  using namespace SweepLimits;
  p.stepMv       = constrain(p.stepMv, kMinStepMv, kMaxStepMv);
  p.pulseAmpMv   = constrain(p.pulseAmpMv, kMinPulseAmpMv, kMaxPulseAmpMv);
  p.intervalMs   = constrain(p.intervalMs, kMinIntervalMs, kMaxIntervalMs);
  p.pulseWidthMs = constrain(p.pulseWidthMs, kMinPulseWidthMs, kMaxPulseWidthMs);
  if (p.pulseWidthMs > p.intervalMs - kMinBaseDwellMs) {
    p.pulseWidthMs = max(kMinPulseWidthMs, p.intervalMs - kMinBaseDwellMs);
  }
  p.equilibrationMs = constrain(p.equilibrationMs, kMinEquilibrationMs, kMaxEquilibrationMs);
  p.fixedRtiaCode = (uint8_t)constrain((int)p.fixedRtiaCode, 0, 7);

  // startV/endV may be in either order — a DPV scan can run ascending
  // (e.g. -0.2V..0.6V) or descending (e.g. 0.6V..-0.2V); AD5941::
  // configureSweep() derives the sweep's actual step direction from
  // whichever one is larger, so neither field needs to be "below" the
  // other here. Both are independently clamped to the valid E range, then
  // nudged apart (preserving whichever direction they already represent,
  // defaulting to ascending only if they started exactly equal) if they'd
  // otherwise be closer together than one step — including the fully
  // degenerate startV==endV case.
  float stepV = p.stepMv / 1000.0f;
  p.startV = constrain(p.startV, kMinV, kMaxV);
  p.endV   = constrain(p.endV, kMinV, kMaxV);
  if (fabsf(p.endV - p.startV) < stepV) {
    bool ascending = p.endV >= p.startV;
    p.endV = ascending ? constrain(p.startV + stepV, kMinV, kMaxV)
                        : constrain(p.startV - stepV, kMinV, kMaxV);
    // If endV got clamped against a rail and still isn't a full step away
    // (only possible when startV itself already sits within one step of
    // that same rail), pull startV back instead so the span is always
    // honoured.
    if (fabsf(p.endV - p.startV) < stepV) {
      p.startV = ascending ? constrain(p.endV - stepV, kMinV, kMaxV)
                            : constrain(p.endV + stepV, kMinV, kMaxV);
    }
  }

  if (p.label.length() > (unsigned)kLabelMax) p.label = p.label.substring(0, kLabelMax);
  if (p.label.length() == 0) p.label = "sample";
}

int sweepPointCount(const SweepParams &p) {
  float stepV = p.stepMv / 1000.0f;
  int n = stepV > 0 ? (int)(fabsf(p.endV - p.startV) / stepV) : 0;
  return n > 0 ? n : 1;
}

void loadSweepParams(SweepParams &p) {
  Preferences prefs;
  // Opened read-write so the namespace is created on first boot instead of
  // logging an NVS "not found" error.
  if (!prefs.begin(kNamespace, false)) return;
  p.startV       = prefs.getFloat("startV", p.startV);
  p.endV         = prefs.getFloat("endV", p.endV);
  p.stepMv       = prefs.getFloat("stepMv", p.stepMv);
  p.pulseAmpMv   = prefs.getFloat("pAmpMv", p.pulseAmpMv);
  p.pulseWidthMs = prefs.getFloat("pWidMs", p.pulseWidthMs);
  p.intervalMs   = prefs.getFloat("intMs", p.intervalMs);
  p.equilibrationMs = prefs.getFloat("equilMs", p.equilibrationMs);
  p.label        = prefs.getString("label", p.label);
  p.autoRange     = prefs.getBool("autoRange", p.autoRange);
  p.fixedRtiaCode = prefs.getUChar("fixRtia", p.fixedRtiaCode);
  prefs.end();
  sanitizeSweepParams(p);
}

void saveSweepParams(const SweepParams &p) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putFloat("startV", p.startV);
  prefs.putFloat("endV", p.endV);
  prefs.putFloat("stepMv", p.stepMv);
  prefs.putFloat("pAmpMv", p.pulseAmpMv);
  prefs.putFloat("pWidMs", p.pulseWidthMs);
  prefs.putFloat("intMs", p.intervalMs);
  prefs.putFloat("equilMs", p.equilibrationMs);
  prefs.putString("label", p.label);
  prefs.putBool("autoRange", p.autoRange);
  prefs.putUChar("fixRtia", p.fixedRtiaCode);
  prefs.end();
}
