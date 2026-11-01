/*
 * screens/screen_summary.h — POST-SCAN SUMMARY (Screen 4) — STUBBED.
 *
 * TODO when building this out for real:
 *   - Header "SWEEP COMPLETE"; peak current (orange) + peak potential
 *     (cyan) computed from sweepBuffer (main.cpp already tracks
 *     sweep.peakI — add a matching peak-voltage tracker alongside it).
 *   - Small static replay of the completed voltammogram: reuse
 *     Renderer's grid-drawing/line-plotting logic from sweepStart/
 *     sweepPoint, just fed the full buffer at once instead of live.
 *   - Filename + duration text.
 *   - Scrollable action row: SAVE (default highlighted) / DISCARD /
 *     RENAME. ROTATE_CW/CCW moves the highlight, SHORT_PRESS executes.
 *   - SAVE: ctx already exposes everything needed (currentFilename(),
 *     the SD-writing logic already lives in main.cpp's writeSweepCsv()
 *     via ctx.stopSweep(..., autosave) — for a manual SAVE-here action
 *     you'll want a dedicated ctx.saveCurrentResult() callback rather
 *     than reusing stopSweep, since by this point the sweep has already
 *     stopped). Then changeTo(HOME).
 *   - DISCARD: changeTo(HOME) without writing (the CSV write only needs
 *     to happen from here if you decouple "sweep stopped" from "sweep
 *     saved" — currently stopSweep() autosaves immediately, so DISCARD
 *     would need to delete that just-written file, or better: stop
 *     autosaving inside stopSweep() and move the SD write into this
 *     screen's SAVE action instead).
 *   - RENAME: a simple text-edit sub-view (character picker driven by
 *     rotate + short-press-to-advance-cursor + long-press-to-confirm)
 *     before falling through to the SAVE path.
 */
#pragma once
#include "../state_machine.h"
#include "../app_context.h"
#include "../drivers/renderer.h"

class ScreenPostScanSummary : public Screen {
public:
  explicit ScreenPostScanSummary(AppContext &ctx) : _ctx(ctx) {}

  void enter() override {
    const char *file = _ctx.currentFilename ? _ctx.currentFilename() : "?";
    _ctx.renderer->clear();

    // Item B — distinguish why the sweep actually ended instead of always
    // showing the same "SWEEP COMPLETE" text, which previously made an
    // instant first-point overload (zero points ever captured) look
    // pixel-for-pixel identical to a real full completion.
    const char *status = _ctx.lastSweepStatus ? _ctx.lastSweepStatus() : "ok";
    bool completed = _ctx.lastSweepCompleted ? _ctx.lastSweepCompleted() : true;
    const char *title;
    uint16_t titleColor;
    if (strcmp(status, "overload") == 0) {
      title = "OVERLOAD";
      titleColor = COLOR_RED;
    } else if (strcmp(status, "error") == 0) {
      title = "ERROR - NO DATA";
      titleColor = COLOR_RED;
    } else if (!completed) {
      title = "STOPPED";
      titleColor = COLOR_GREY;
    } else {
      title = "SWEEP COMPLETE";
      titleColor = COLOR_GREEN;
    }
    _ctx.renderer->centeredTitle(title, 20, titleColor, 1);
    if (strcmp(status, "overload") == 0) {
      _ctx.renderer->printLine(2, "TRY A LOWER RTIA RANGE", COLOR_GREY);
    }

    char buf[40];
    snprintf(buf, sizeof(buf), "FILE: %s", file);
    _ctx.renderer->printLine(5, buf, COLOR_WHITE);
    _ctx.renderer->printLine(7, "TODO: peak/replay/save row", COLOR_GREY);
    _ctx.renderer->printLine(11, "HOLD: BACK (=HOME)", COLOR_GREY);
  }

  void onInput(const InputEvent &ev) override {
    if (ev.type == InputType::LONG_PRESS) {
      _ctx.stateMachine->changeTo(ScreenId::HOME);
    }
  }

private:
  AppContext &_ctx;
};
