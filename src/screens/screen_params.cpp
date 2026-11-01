#include "screen_params.h"
#include "../drivers/renderer.h"

// Text-entry alphabet for the label. Kept to characters that are safe in a
// FAT filename since the label becomes part of the CSV name.
static const char kCharset[] = "abcdefghijklmnopqrstuvwxyz0123456789_-ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const int kCharsetLen = sizeof(kCharset) - 1;

// Mirrors AD5941::kRtiaOhmsTable (HSRTIACON[3:0] codes 0-7, Table 38 p.52)
// for display only — kept as a local copy rather than reaching into the AFE
// driver, same "screens only touch AppContext" convention as the rest of
// this file. Keep in sync if the AD5941 table ever changes.
static const float kRtiaOhmsTable[8] = {
  200.0f, 1000.0f, 5000.0f, 10000.0f, 20000.0f, 40000.0f, 80000.0f, 160000.0f
};

// Screen layout (10px text lines, 12 of them): 0 = title, 1..8 = fields
// (F_COUNT=8 now fills this exactly, no gap before the estimate line),
// 9 = point/duration estimate, 10..11 = key hints.
static const int kFirstFieldLine = 1;
static const int kEstimateLine = 9;
static const int kHintLine1 = 10;
static const int kHintLine2 = 11;
static const int kNameCols = 10; // width of the field-name column, in characters

static float roundTo(float v, float step) { return roundf(v / step) * step; }

void ScreenParamEntry::enter() {
  _work = _ctx.savedParams ? *_ctx.savedParams : SweepParams();
  sanitizeSweepParams(_work);
  _sel = 0;
  _editing = false;
  _fine = true;
  drawAll();
}

void ScreenParamEntry::formatRow(int field, char *buf, size_t len) const {
  static const char *kNames[F_COUNT] = {"Start", "Stop", "Step", "Pulse Amp", "Pulse Wid",
                                          "Interval", "RTIA", "Label"};
  char value[24];
  switch (field) {
    case F_START:       snprintf(value, sizeof(value), "%.3f V", _work.startV); break;
    case F_STOP:        snprintf(value, sizeof(value), "%.3f V", _work.endV); break;
    case F_STEP:        snprintf(value, sizeof(value), "%.0f mV", _work.stepMv); break;
    case F_PULSE_AMP:   snprintf(value, sizeof(value), "%.0f mV", _work.pulseAmpMv); break;
    case F_PULSE_WIDTH: snprintf(value, sizeof(value), "%.0f ms", _work.pulseWidthMs); break;
    case F_INTERVAL:    snprintf(value, sizeof(value), "%.0f ms", _work.intervalMs); break;
    case F_RTIA:
      if (_work.autoRange) {
        snprintf(value, sizeof(value), "AUTO");
      } else {
        float ohms = kRtiaOhmsTable[_work.fixedRtiaCode];
        if (ohms >= 1000.0f) snprintf(value, sizeof(value), "%.0f kohm", ohms / 1000.0f);
        else snprintf(value, sizeof(value), "%.0f ohm", ohms);
      }
      break;
    default:            snprintf(value, sizeof(value), "%s", _work.label.c_str()); break;
  }
  snprintf(buf, len, "%-*s%s", kNameCols, kNames[field], value);
}

void ScreenParamEntry::drawRow(int field) {
  char buf[27];
  formatRow(field, buf, sizeof(buf));
  bool selected = (field == _sel);
  bool editing = selected && _editing;
  int line = kFirstFieldLine + field;
  _ctx.renderer->menuRow(line, buf, selected, editing);
  if (editing && field == F_LABEL) {
    _ctx.renderer->underlineChar(line, kNameCols + _labelCursor, COLOR_BG);
  }
}

void ScreenParamEntry::drawEstimate() {
  int points = sweepPointCount(_work);
  float seconds = points * _work.intervalMs / 1000.0f;
  char buf[27];
  snprintf(buf, sizeof(buf), "%d pts   %.1f s", points, seconds);
  _ctx.renderer->printLine(kEstimateLine, buf, COLOR_CYAN);
}

void ScreenParamEntry::drawHints() {
  const char *h1, *h2;
  if (!_editing) {
    h1 = "TURN:SELECT PRESS:EDIT";
    h2 = "HOLD:SAVE  STOP:CANCEL";
  } else if (_sel == F_LABEL) {
    h1 = "TURN:CHAR PRESS:NEXT";
    h2 = "2xCLICK:DONE HOLD:UNDO";
  } else if (_sel == F_RTIA) {
    h1 = "TURN:CYCLE RTIA";
    h2 = "PRESS:OK  HOLD:UNDO";
  } else {
    h1 = "TURN:CHANGE PRESS:OK";
    h2 = _fine ? "FINE  2xCLICK:COARSE" : "COARSE  2xCLICK:FINE";
  }
  _ctx.renderer->printLine(kHintLine1, h1, COLOR_GREY);
  _ctx.renderer->printLine(kHintLine2, h2, COLOR_GREY);
}

void ScreenParamEntry::drawAll() {
  _ctx.renderer->clear();
  _ctx.renderer->printLine(0, "DPV PARAMETERS", COLOR_GREEN);
  for (int f = 0; f < F_COUNT; f++) drawRow(f);
  drawEstimate();
  drawHints();
}

void ScreenParamEntry::startEdit() {
  _beforeEdit = _work;
  _editing = true;
  if (_sel == F_LABEL) {
    if (_work.label.length() == 0) _work.label = "a";
    _labelCursor = 0;
  }
  drawRow(_sel);
  drawHints();
}

// keep=false restores the snapshot taken when the edit started.
void ScreenParamEntry::finishEdit(bool keep) {
  if (!keep) _work = _beforeEdit;
  sanitizeSweepParams(_work);
  _editing = false;
  drawAll(); // an edit can move other fields too (e.g. the start/stop/interval limits), so redraw them all
}

void ScreenParamEntry::adjust(int dir) {
  using namespace SweepLimits;
  switch (_sel) {
    case F_START: {
      // Not clamped against endV here — a scan can run in either
      // direction (e.g. 0.6V down to -0.2V), so startV/endV don't need to
      // stay in a fixed order while live-editing. finishEdit() calls
      // sanitizeSweepParams() right after, which is what actually
      // enforces the minimum one-step separation, in whichever direction
      // the two fields already represent.
      float v = roundTo(_work.startV + dir * (_fine ? 0.005f : 0.050f), 0.001f);
      _work.startV = constrain(v, kMinV, kMaxV);
      break;
    }
    case F_STOP: {
      float v = roundTo(_work.endV + dir * (_fine ? 0.005f : 0.050f), 0.001f);
      _work.endV = constrain(v, kMinV, kMaxV);
      break;
    }
    case F_STEP:
      _work.stepMv = constrain(_work.stepMv + dir * (_fine ? 1.0f : 5.0f), kMinStepMv, kMaxStepMv);
      break;
    case F_PULSE_AMP:
      _work.pulseAmpMv = constrain(_work.pulseAmpMv + dir * (_fine ? 1.0f : 5.0f), kMinPulseAmpMv, kMaxPulseAmpMv);
      break;
    case F_PULSE_WIDTH: {
      float hi = min(kMaxPulseWidthMs, _work.intervalMs - kMinBaseDwellMs);
      _work.pulseWidthMs = constrain(_work.pulseWidthMs + dir * (_fine ? 5.0f : 25.0f), kMinPulseWidthMs, hi);
      break;
    }
    case F_INTERVAL: {
      float lo = max(kMinIntervalMs, _work.pulseWidthMs + kMinBaseDwellMs);
      _work.intervalMs = constrain(_work.intervalMs + dir * (_fine ? 50.0f : 250.0f), lo, kMaxIntervalMs);
      break;
    }
    case F_RTIA: {
      // One linear cycle of 9 states: index 0 = AUTO, 1..8 = fixedRtiaCode
      // 0..7 (200ohm..160kohm, kRtiaOhmsTable above) — TURN steps through
      // all of them in order and wraps, same "TURN:CHANGE" idiom as every
      // other field here, rather than a separate auto/fixed toggle plus a
      // nested resistor-value scroll.
      int idx = _work.autoRange ? 0 : (int)_work.fixedRtiaCode + 1;
      idx = (idx + dir + 9) % 9;
      _work.autoRange = (idx == 0);
      if (!_work.autoRange) _work.fixedRtiaCode = (uint8_t)(idx - 1);
      break;
    }
    default:
      adjustLabel(dir);
      break;
  }
  drawRow(_sel);
  drawEstimate();
}

void ScreenParamEntry::adjustLabel(int dir) {
  if (_labelCursor >= (int)_work.label.length()) return;
  char c = _work.label[_labelCursor];
  int idx = 0;
  for (int i = 0; i < kCharsetLen; i++) {
    if (kCharset[i] == c) { idx = i; break; }
  }
  idx = (idx + dir + kCharsetLen) % kCharsetLen;
  _work.label.setCharAt(_labelCursor, kCharset[idx]);
}

void ScreenParamEntry::saveAndExit() {
  sanitizeSweepParams(_work);
  if (_ctx.savedParams) *_ctx.savedParams = _work;
  saveSweepParams(_work);
  _ctx.stateMachine->changeTo(ScreenId::HOME);
}

void ScreenParamEntry::onInput(const InputEvent &ev) {
  if (!_editing) {
    switch (ev.type) {
      case InputType::ROTATE_CW:
      case InputType::ROTATE_CCW: {
        int prev = _sel;
        _sel = (_sel + (ev.type == InputType::ROTATE_CW ? 1 : F_COUNT - 1)) % F_COUNT;
        drawRow(prev);
        drawRow(_sel);
        break;
      }
      case InputType::SHORT_PRESS:
        startEdit();
        break;
      case InputType::LONG_PRESS:
        saveAndExit();
        break;
      case InputType::STOP_BTN:
        _ctx.stateMachine->changeTo(ScreenId::HOME); // discard: _work is only copied out on save
        break;
      default:
        break;
    }
    return;
  }

  // ---- editing a field ----
  bool isLabel = (_sel == F_LABEL);
  switch (ev.type) {
    case InputType::ROTATE_CW:
      adjust(+1);
      break;
    case InputType::ROTATE_CCW:
      adjust(-1);
      break;
    case InputType::SHORT_PRESS:
      if (!isLabel) {
        finishEdit(true);
      } else if (_labelCursor + 1 < (int)_work.label.length()) {
        _labelCursor++;
        drawRow(_sel);
      } else if ((int)_work.label.length() < SweepLimits::kLabelMax) {
        _work.label += _work.label[_labelCursor]; // new character starts as a copy of the previous one
        _labelCursor++;
        drawRow(_sel);
      } else {
        finishEdit(true); // at the max length: nothing left to add
      }
      break;
    case InputType::DOUBLE_CLICK:
      if (isLabel) {
        _work.label = _work.label.substring(0, _labelCursor + 1); // finish: label ends at the cursor
        finishEdit(true);
      } else {
        _fine = !_fine;
        drawHints();
      }
      break;
    case InputType::LONG_PRESS:
      finishEdit(false); // undo
      break;
    default:
      break;
  }
}
