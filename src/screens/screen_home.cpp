#include "screen_home.h"
#include "../drivers/renderer.h"

void ScreenHome::enter() {
  // A sweep may already be running if it was started from the web
  // dashboard (or from Home, navigated away from, and back) — resync to
  // SWEEPING rather than stomping it with the idle "READY" frame. The
  // running sweep's own renderer.sweepStart()/sweepPoint() calls (driven
  // by main.cpp's serviceSweep()) will repaint the chrome/grid on their
  // own next tick.
  if (_ctx.isSweepRunning && _ctx.isSweepRunning()) {
    _sub = SubState::SWEEPING;
    return;
  }
  _sub = SubState::IDLE;
  _footerShowParams = true;
  _lastFooterToggleMs = millis();
  drawIdleFrame();
}

void ScreenHome::exit() {
  // Nothing to tear down — the renderer owns no per-screen resources.
}

void ScreenHome::drawIdleFrame() {
  bool wifi = _ctx.wifiUp ? _ctx.wifiUp() : false;
  bool sd = _ctx.sdReady ? _ctx.sdReady() : false;
  int batt = _ctx.batteryPct ? _ctx.batteryPct() : 100;
  const char *tech = _ctx.currentTechnique ? _ctx.currentTechnique() : "DPV";
  const char *rtia = _ctx.rtiaText ? _ctx.rtiaText() : "?";

  _ctx.renderer->clear();
  _ctx.renderer->drawChrome(tech, wifi, sd, batt, rtia);
  _ctx.renderer->updateReadout(0.0f, 0.0f);
  _ctx.renderer->centeredTitle("READY", 60, COLOR_GREY, 1);
  // Where to point a browser to reach the web dashboard.
  if (_ctx.ipAddress) _ctx.renderer->centeredTitle(_ctx.ipAddress(), 78, COLOR_CYAN, 1);
  drawFooterHint();
}

void ScreenHome::drawFooterHint() {
  const char *text = _footerShowParams ? "PRESS: PARAMETERS" : "HOLD: FILES";
  _ctx.renderer->printLine(11, text, COLOR_GREY);
}

void ScreenHome::update() {
  switch (_sub) {
    case SubState::IDLE: {
      unsigned long now = millis();
      if (now - _lastFooterToggleMs >= FOOTER_TOGGLE_MS) {
        _lastFooterToggleMs = now;
        _footerShowParams = !_footerShowParams;
        drawFooterHint();
      }
      break;
    }

    case SubState::SWEEPING:
      // Point-by-point rendering happens in main.cpp's serviceSweep(),
      // gated on `stateMachine.currentId() == ScreenId::HOME` — this
      // screen doesn't duplicate that pacing logic. It just watches for
      // the sweep ending on its own (ran to completion, or was aborted via
      // STOP_BTN — see onInput() below) so it can hand off to the summary
      // screen.
      if (_ctx.isSweepRunning && !_ctx.isSweepRunning()) {
        _sub = SubState::IDLE;
        _ctx.stateMachine->changeTo(ScreenId::POST_SCAN_SUMMARY);
      }
      break;
  }
}

void ScreenHome::onInput(const InputEvent &ev) {
  switch (_sub) {
    case SubState::IDLE:
      if (ev.type == InputType::SHORT_PRESS) {
        _ctx.stateMachine->changeTo(ScreenId::PARAM_ENTRY);
      } else if (ev.type == InputType::LONG_PRESS) {
        _ctx.stateMachine->changeTo(ScreenId::FILE_BROWSER);
      } else if (ev.type == InputType::DOUBLE_CLICK) {
        // Item I — Device Settings (QR code etc.) was registered in the
        // state machine but had no navigation entry point anywhere in the
        // UI. DOUBLE_CLICK is free in this IDLE case (SHORT_PRESS/
        // LONG_PRESS/START_BTN already used), so it's the new entry.
        _ctx.stateMachine->changeTo(ScreenId::DEVICE_SETTINGS);
      } else if (ev.type == InputType::START_BTN) {
        // Only start if the AFE is actually idle — a sweep may already be
        // running (e.g. kicked off from the web dashboard) even though
        // this screen's own substate hasn't caught up yet; in that case
        // just resync instead of re-entering startSweep().
        if (_ctx.isSweepRunning && _ctx.isSweepRunning()) {
          _sub = SubState::SWEEPING;
        } else if (_ctx.startSweep && _ctx.savedParams) {
          // Item B — only treat this as "now sweeping" if it actually
          // started; startSweep() can bail out (already running, STOP
          // pressed mid-probe, configureSweep() refusing the timing)
          // without ever reaching afe.startSweep(). Previously this was
          // set unconditionally, which is what let a failed start still
          // look like a real sweep that "instantly completed" with zero
          // points on the next screen.
          if (_ctx.startSweep(*_ctx.savedParams)) { // uses last-saved parameters
            _sub = SubState::SWEEPING;
          }
        }
      }
      break;

    case SubState::SWEEPING:
      if (ev.type == InputType::STOP_BTN) {
        // Dedicated hardware button, not the encoder — a press is already
        // a deliberate abort, so stop immediately rather than asking for
        // confirmation. stopSweep() still flushes whatever partial data
        // was captured to SD before the state machine hands off below.
        if (_ctx.stopSweep) _ctx.stopSweep(false);
        _sub = SubState::IDLE;
        _ctx.stateMachine->changeTo(ScreenId::POST_SCAN_SUMMARY);
      }
      break;
  }
}
