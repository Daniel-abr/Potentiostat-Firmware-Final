/*
 * screens/screen_home.h — HOME / LIVE MEASUREMENT screen.
 *
 * Ports the layout/logic of potentiostat_dashboard_clinical.html (and the
 * pixel-art reference in UI_design.txt) onto the physical TFT: mode +
 * status icons, live current/voltage readout, live voltammogram trace,
 * progress bar, elapsed timer, filename, and a footer hint that
 * alternates between "PRESS: PARAMETERS" and "HOLD: FILES" while idle.
 *
 * Point-by-point plotting during an active sweep is still driven by
 * main.cpp's serviceSweep() (which already had this logic, paced to the
 * real acquisition rate) — it calls straight into Renderer, gated on this
 * screen being the active one. This screen owns everything else: the
 * idle frame, the footer hint animation, and input handling (starting a
 * sweep, opening Params/Files).
 *
 * START_BTN/STOP_BTN are the two dedicated hardware buttons (not the
 * encoder), so a press is already a deliberate, unambiguous action —
 * STOP_BTN aborts an in-progress sweep immediately (no confirm step) and
 * main.cpp's stopSweep() still flushes whatever partial data was captured
 * to SD before returning to idle. START_BTN is a no-op if a sweep is
 * already running (e.g. started from the web dashboard while this screen
 * sat idle) rather than re-entering startSweep().
 */
#pragma once
#include "../state_machine.h"
#include "../app_context.h"

class ScreenHome : public Screen {
public:
  explicit ScreenHome(AppContext &ctx) : _ctx(ctx) {}

  void enter() override;
  void exit() override;
  void update() override;
  void onInput(const InputEvent &ev) override;

  // True while a sweep is active and this screen should let
  // main.cpp/serviceSweep() drive point-by-point rendering.
  bool isSweeping() const { return _sub == SubState::SWEEPING; }

private:
  enum class SubState { IDLE, SWEEPING };
  SubState _sub = SubState::IDLE;

  unsigned long _lastFooterToggleMs = 0;
  bool _footerShowParams = true;
  static const unsigned long FOOTER_TOGGLE_MS = 2000;

  void drawIdleFrame();
  void drawFooterHint();

  AppContext &_ctx;
};
