#include "screen_boot.h"
#include "../drivers/renderer.h"

#define FW_VERSION "v0.1.0"

void ScreenBoot::enter() {
  _stage = Stage::WORDMARK;
  _stageStartMs = millis();
  _anyFailure = false;
  drawWordmark();
}

void ScreenBoot::drawWordmark() {
  _ctx.renderer->clear();
  // Title/logo colour only (confirmed with the user) — the smaller version
  // restatement line below, and the "OPENSTAT "+version header line drawn
  // later in update()'s WORDMARK stage, stay COLOR_WHITE, unchanged.
  _ctx.renderer->centeredTitle("OPENSTAT", 40, COLOR_ORANGE, 2);
  _ctx.renderer->centeredTitle(FW_VERSION, 66, COLOR_GREY, 1);
}

void ScreenBoot::drawTestLine(int line, const char *label, bool ok) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%-10s %s", label, ok ? "OK" : "FAIL");
  _ctx.renderer->printLine(line, buf, ok ? COLOR_GREEN : COLOR_RED);
}

void ScreenBoot::drawProgress(int pct) {
  _ctx.renderer->progressBar(4, TFT_H - 12, TFT_W - 8, 6, pct, COLOR_GREEN);
}

void ScreenBoot::update() {
  unsigned long now = millis();

  // FAILED_WAIT is a hold/wait state, not part of the paced reveal below —
  // it must NOT have _stageStartMs clobbered by the general pacing reset,
  // or the "hold for HOLD_AFTER_DONE_MS then advance" check below never
  // sees more than ~0ms elapsed and auto-advance never fires.
  if (_stage == Stage::FAILED_WAIT) {
    if (_autoAdvance && (now - _stageStartMs) >= HOLD_AFTER_DONE_MS) {
      _ctx.stateMachine->changeTo(ScreenId::HOME);
    }
    return; // otherwise: waiting on input (self-test failure) — nothing to do
  }

  if (now - _stageStartMs < STAGE_DELAY_MS) return; // pace the reveal
  _stageStartMs = now;

  switch (_stage) {
    case Stage::WORDMARK:
      _ctx.renderer->clear();
      _ctx.renderer->printLine(0, "OPENSTAT " FW_VERSION, COLOR_WHITE);
      _ctx.renderer->printLine(1, "Running self-test...", COLOR_GREY);
      drawProgress(10);
      _stage = Stage::TEST_AFE;
      break;

    case Stage::TEST_AFE:
      drawTestLine(3, "AD5941", _ctx.afeOk);
      if (!_ctx.afeOk) _anyFailure = true;
      drawProgress(40);
      _stage = Stage::TEST_SD;
      break;

    case Stage::TEST_SD:
      drawTestLine(4, "SD CARD", _ctx.sdOk);
      if (!_ctx.sdOk) _anyFailure = true;
      drawProgress(70);
      _stage = Stage::TEST_WIFI;
      break;

    case Stage::TEST_WIFI: {
      bool wifi = _ctx.wifiUp ? _ctx.wifiUp() : false;
      drawTestLine(5, "WI-FI", wifi);
      // Wi-Fi is non-critical (AP fallback still lets the web UI work,
      // and the device is fully usable standalone), so don't fail boot
      // over it — just report it.
      drawProgress(100);
      _stage = Stage::DONE;
      break;
    }

    case Stage::DONE:
      if (_anyFailure) {
        _ctx.renderer->printLine(7, "PRESS: CONTINUE ANYWAY", COLOR_RED);
        _autoAdvance = false;
      } else {
        _ctx.renderer->printLine(7, "READY", COLOR_GREEN);
        _autoAdvance = true;
      }
      _stage = Stage::FAILED_WAIT; // now acts as "hold, then maybe auto-advance"
      _stageStartMs = now;         // start the hold timer fresh, see guard above
      break;

    case Stage::FAILED_WAIT:
      break; // unreachable — handled by the early return at the top of update()
  }
}

void ScreenBoot::onInput(const InputEvent &ev) {
  if (_stage != Stage::FAILED_WAIT || _autoAdvance) return; // only react once tests are done+failed
  if (ev.type == InputType::SHORT_PRESS || ev.type == InputType::START_BTN) {
    _ctx.stateMachine->changeTo(ScreenId::HOME);
  }
}
