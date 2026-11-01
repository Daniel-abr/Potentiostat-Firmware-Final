#include "screen_files.h"
#include "../drivers/renderer.h"

// List layout (10px text lines): 0 = header, 1..kListRows = files,
// 10 = selected file's peak, 11 = key hints.
static const int kListRows = 9;
static const int kDetailLine = 10;
static const int kHintLine = 11;

// Viewer plot area (pixels) and the axis-label row beneath it.
static const int kPlotX = 4, kPlotY = 13, kPlotW = 152, kPlotH = 74;
static const int kAxisLabelY = 90;
static const uint16_t kGridColor = 0x2965; // same dim grey the live graph's grid uses

// ---- CSV pass 1: axis ranges + peak ----------------------------------------
struct PlotStats {
  int n = 0;
  float minV = 1e9f, maxV = -1e9f;
  float minI = 1e9f, maxI = -1e9f;
  float peakV = 0;
};

static void statsVisitor(float v, float i, void *user) {
  PlotStats *s = (PlotStats *)user;
  s->n++;
  if (v < s->minV) s->minV = v;
  if (v > s->maxV) s->maxV = v;
  if (i < s->minI) s->minI = i;
  if (i > s->maxI) { s->maxI = i; s->peakV = v; }
}

// ---- CSV pass 2: draw the trace ------------------------------------------
struct PlotDraw {
  Renderer *r;
  float minV, maxV, minI, maxI;
  int prevX = -1, prevY = -1;
};

static int plotX(const PlotDraw &p, float v) {
  float f = (v - p.minV) / (p.maxV - p.minV);
  return kPlotX + 1 + (int)(f * (kPlotW - 3));
}

static int plotY(const PlotDraw &p, float i) {
  float f = (i - p.minI) / (p.maxI - p.minI);
  return kPlotY + kPlotH - 2 - (int)(f * (kPlotH - 3));
}

static void drawVisitor(float v, float i, void *user) {
  PlotDraw *p = (PlotDraw *)user;
  int x = plotX(*p, v), y = plotY(*p, i);
  if (p->prevX >= 0) p->r->drawLine(p->prevX, p->prevY, x, y, COLOR_GREEN);
  p->prevX = x;
  p->prevY = y;
}

// ---------------------------------------------------------------------------

int ScreenFileBrowser::count() const {
  return _ctx.savedResultsCount ? _ctx.savedResultsCount() : 0;
}

void ScreenFileBrowser::enter() {
  _mode = Mode::LIST;
  _sel = 0;
  _top = 0;
  drawList();
}

void ScreenFileBrowser::drawListRow(int idx) {
  int line = 1 + (idx - _top);
  if (line < 1 || line > kListRows) return;
  char file[64], peak[24], row[27];
  if (!_ctx.savedResultAt || !_ctx.savedResultAt(idx, file, sizeof(file), peak, sizeof(peak))) return;
  snprintf(row, sizeof(row), "%s", file);
  _ctx.renderer->menuRow(line, row, idx == _sel, false);
}

void ScreenFileBrowser::drawListDetail() {
  char file[64], peak[24], buf[27];
  if (_ctx.savedResultAt && _ctx.savedResultAt(_sel, file, sizeof(file), peak, sizeof(peak))) {
    snprintf(buf, sizeof(buf), "Peak: %s", peak);
    _ctx.renderer->printLine(kDetailLine, buf, COLOR_ORANGE);
  }
}

void ScreenFileBrowser::drawList() {
  int n = count();
  _ctx.renderer->clear();
  char buf[27];
  snprintf(buf, sizeof(buf), "SAVED RESULTS (%d)", n);
  _ctx.renderer->printLine(0, buf, COLOR_GREEN);

  if (n == 0) {
    bool sd = _ctx.sdReady ? _ctx.sdReady() : false;
    _ctx.renderer->printLine(4, sd ? "No saved sweeps yet" : "SD card not ready", sd ? COLOR_GREY : COLOR_RED);
    _ctx.renderer->printLine(kHintLine, "HOLD:BACK", COLOR_GREY);
    return;
  }

  for (int idx = _top; idx < n && idx < _top + kListRows; idx++) drawListRow(idx);
  drawListDetail();
  _ctx.renderer->printLine(kHintLine, "PRESS:VIEW  HOLD:BACK", COLOR_GREY);
}

void ScreenFileBrowser::showList() {
  _mode = Mode::LIST;
  if (_sel < _top) _top = _sel;
  if (_sel >= _top + kListRows) _top = _sel - kListRows + 1;
  drawList();
}

void ScreenFileBrowser::selectInList(int newSel) {
  if (count() <= 0) return;
  newSel = constrain(newSel, 0, count() - 1);
  if (newSel == _sel) return;
  int prev = _sel;
  _sel = newSel;
  if (_sel < _top || _sel >= _top + kListRows) {
    if (_sel < _top) _top = _sel;
    else _top = _sel - kListRows + 1;
    drawList();
  } else {
    drawListRow(prev);
    drawListRow(_sel);
    drawListDetail();
  }
}

void ScreenFileBrowser::selectInViewer(int newSel) {
  if (count() <= 0) return;
  newSel = constrain(newSel, 0, count() - 1);
  if (newSel == _sel) return;
  _sel = newSel;
  drawViewer();
}

void ScreenFileBrowser::drawViewer() {
  Renderer &r = *_ctx.renderer;
  char file[64], peak[24], buf[27];
  r.clear();
  if (!_ctx.savedResultAt || !_ctx.savedResultAt(_sel, file, sizeof(file), peak, sizeof(peak))) {
    r.printLine(4, "File not found", COLOR_RED);
    r.printLine(kHintLine, "PRESS:BACK", COLOR_GREY);
    return;
  }
  snprintf(buf, sizeof(buf), "%s", file);
  r.printLine(0, buf, COLOR_WHITE);

  PlotStats st;
  bool ok = _ctx.forEachResultPoint && _ctx.forEachResultPoint(file, statsVisitor, &st);
  if (!ok || st.n < 2) {
    r.printLine(4, ok ? "Not enough data" : "Can't read file", COLOR_RED);
    r.printLine(kHintLine, "TURN:FILE PRESS:BACK", COLOR_GREY);
    return;
  }

  // Axis ranges with a little headroom so the trace never touches the frame.
  PlotDraw pd;
  pd.r = &r;
  float spanV = st.maxV - st.minV;
  if (spanV < 1e-6f) spanV = 0.1f;
  pd.minV = st.minV;
  pd.maxV = st.minV + spanV;
  float spanI = st.maxI - st.minI;
  if (spanI < 1e-3f) spanI = 1.0f;
  pd.minI = st.minI - 0.05f * spanI;
  pd.maxI = st.maxI + 0.05f * spanI;

  r.drawFrame(kPlotX, kPlotY, kPlotW, kPlotH, COLOR_GREY);
  if (pd.minI < 0 && pd.maxI > 0) {
    int yz = plotY(pd, 0.0f);
    r.drawLine(kPlotX + 1, yz, kPlotX + kPlotW - 2, yz, kGridColor);
  }
  _ctx.forEachResultPoint(file, drawVisitor, &pd);

  // Potential axis end labels under the plot.
  snprintf(buf, sizeof(buf), "%.3f", st.minV);
  r.printAt(kPlotX, kAxisLabelY, buf, COLOR_CYAN);
  snprintf(buf, sizeof(buf), "%.3f V", st.maxV);
  r.printAt(kPlotX + kPlotW - (int)strlen(buf) * 6, kAxisLabelY, buf, COLOR_CYAN);

  snprintf(buf, sizeof(buf), "%d pts", st.n);
  r.printAt(kPlotX + kPlotW / 2 - (int)strlen(buf) * 3, kAxisLabelY, buf, COLOR_GREY);

  snprintf(buf, sizeof(buf), "Peak %.1f nA @ %.3f V", st.maxI, st.peakV);
  r.printLine(kDetailLine, buf, COLOR_ORANGE);
  r.printLine(kHintLine, "TURN:FILE PRESS:BACK", COLOR_GREY);
}

void ScreenFileBrowser::onInput(const InputEvent &ev) {
  if (_mode == Mode::LIST) {
    switch (ev.type) {
      case InputType::ROTATE_CW:  selectInList(_sel + 1); break;
      case InputType::ROTATE_CCW: selectInList(_sel - 1); break;
      case InputType::SHORT_PRESS:
        if (count() > 0) {
          _mode = Mode::VIEW;
          drawViewer();
        }
        break;
      case InputType::LONG_PRESS:
      case InputType::STOP_BTN:
        _ctx.stateMachine->changeTo(ScreenId::HOME);
        break;
      default: break;
    }
    return;
  }

  // Viewer
  switch (ev.type) {
    case InputType::ROTATE_CW:  selectInViewer(_sel + 1); break;
    case InputType::ROTATE_CCW: selectInViewer(_sel - 1); break;
    case InputType::SHORT_PRESS:
    case InputType::LONG_PRESS:
    case InputType::STOP_BTN:
      showList();
      break;
    default: break;
  }
}
