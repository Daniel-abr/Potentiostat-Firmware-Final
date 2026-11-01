/*
 * screens/screen_files.h — SAVED RESULTS / FILE BROWSER + WAVEFORM VIEWER (Screen 5).
 *
 * Lists the sweeps saved on the SD card (newest first) and plots any of them
 * as a current-vs-potential voltammogram read straight from its CSV.
 *
 *   list:   TURN = scroll   PRESS = view   HOLD or STOP button = back to Home
 *   viewer: TURN = previous/next file   PRESS, HOLD or STOP = back to the list
 *
 * The list comes from main.cpp's savedResults vector (the SD-persisted
 * /results/index.json), so only sweeps recorded in that index show up. The
 * CSV is streamed twice through ctx.forEachResultPoint() — once for the axis
 * ranges/peak, once to draw — so a sweep of any length plots without being
 * held in RAM.
 *
 * Not implemented: deleting a file (would need a ctx.deleteSavedResult()
 * callback that removes the CSV and rewrites index.json) and a metadata
 * header in the CSV for showing the sweep parameters.
 */
#pragma once
#include "../state_machine.h"
#include "../app_context.h"

class ScreenFileBrowser : public Screen {
public:
  explicit ScreenFileBrowser(AppContext &ctx) : _ctx(ctx) {}

  void enter() override;
  void onInput(const InputEvent &ev) override;

private:
  enum class Mode { LIST, VIEW };

  AppContext &_ctx;
  Mode _mode = Mode::LIST;
  int _sel = 0;   // selected file (index into the saved-results list, 0 = newest)
  int _top = 0;   // first list row shown

  int count() const;
  void drawList();
  void drawListRow(int idx);
  void drawListDetail();
  void drawViewer();
  void showList();
  void selectInList(int newSel);
  void selectInViewer(int newSel);
};
