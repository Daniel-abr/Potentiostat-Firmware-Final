/*
 * screens/screen_params.h — DPV PARAMETER ENTRY (Screen 2).
 *
 * Edits the waveform the next sweep uses: start/stop potential, step,
 * pulse amplitude, pulse width, interval time, RTIA range (SweepParams::
 * autoRange/fixedRtiaCode, combined into one field — TURN cycles AUTO, then
 * each of the 8 fixed RTIA codes 200ohm..160kohm in order), and the sample
 * label, with a live point-count / duration estimate. Values are saved to
 * flash (NVS, see sweep_params.h) and into *ctx.savedParams, which is what
 * Home's Start button runs. Editing and saving here is also the fix for
 * autoRange/fixedRtiaCode drifting out of sync with SweepParams' own
 * compiled default — previously this screen had no control for either, so
 * re-saving any other field here would silently persist whatever values
 * happened to already be loaded.
 *
 * Controls
 *   list:  TURN = select field   PRESS = edit   HOLD = save + back to Home
 *          STOP button = discard changes + back to Home
 *   edit (numbers):  TURN = change   2x-CLICK = toggle fine/coarse step
 *                    PRESS = accept   HOLD = undo this edit
 *   edit (RTIA):     TURN = cycle AUTO / 200ohm .. 160kohm   PRESS = accept
 *                    HOLD = undo
 *   edit (label):    TURN = change character   PRESS = next character
 *                    (past the last one it adds a character, up to the max)
 *                    2x-CLICK = finish (label ends at the cursor)   HOLD = undo
 */
#pragma once
#include "../state_machine.h"
#include "../app_context.h"
#include "../sweep_params.h"

class ScreenParamEntry : public Screen {
public:
  explicit ScreenParamEntry(AppContext &ctx) : _ctx(ctx) {}

  void enter() override;
  void onInput(const InputEvent &ev) override;

private:
  enum Field { F_START, F_STOP, F_STEP, F_PULSE_AMP, F_PULSE_WIDTH, F_INTERVAL, F_RTIA, F_LABEL, F_COUNT };

  AppContext &_ctx;
  SweepParams _work;         // the copy being edited
  SweepParams _beforeEdit;   // snapshot taken when an edit starts, for HOLD = undo
  int _sel = 0;
  bool _editing = false;
  bool _fine = true;         // fine vs coarse step while editing a number
  int _labelCursor = 0;

  void drawAll();
  void drawRow(int field);
  void drawEstimate();
  void drawHints();
  void formatRow(int field, char *buf, size_t len) const;

  void startEdit();
  void finishEdit(bool keep);
  void adjust(int direction);
  void adjustLabel(int direction);
  void saveAndExit();
};
