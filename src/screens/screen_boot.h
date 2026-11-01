/*
 * screens/screen_boot.h — BOOT / SPLASH screen.
 *
 * Shows the wordmark + firmware version, then reveals three self-test
 * results (AD5941 SPI, SD card, Wi-Fi) one at a time for a readable
 * "diagnostics running" effect, then auto-advances to Home.
 *
 * The actual tests (afe.reset(), SD.begin(), WiFi connect/AP fallback)
 * already ran synchronously in main.cpp's setup() before the state
 * machine switches to this screen — their pass/fail results are simply
 * *staged for display* here via a millis() timer, non-blocking. If you
 * later make any of those tests genuinely async, update() is exactly
 * where you'd poll them instead of reading a precomputed bool.
 */
#pragma once
#include "../state_machine.h"
#include "../app_context.h"

class ScreenBoot : public Screen {
public:
  explicit ScreenBoot(AppContext &ctx) : _ctx(ctx) {}

  void enter() override;
  void update() override;
  void onInput(const InputEvent &ev) override;

private:
  enum class Stage { WORDMARK, TEST_AFE, TEST_SD, TEST_WIFI, DONE, FAILED_WAIT };

  Stage _stage = Stage::WORDMARK;
  unsigned long _stageStartMs = 0;
  static const unsigned long STAGE_DELAY_MS = 350; // pacing between reveals
  static const unsigned long HOLD_AFTER_DONE_MS = 500;

  bool _anyFailure = false;
  bool _autoAdvance = false; // true once tests passed cleanly and we're just holding "READY" briefly

  void drawWordmark();
  void drawTestLine(int line, const char *label, bool ok);
  void drawProgress(int pct);

  AppContext &_ctx;
};
