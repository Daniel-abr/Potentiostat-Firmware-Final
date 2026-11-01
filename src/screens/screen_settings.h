/*
 * screens/screen_settings.h — DEVICE SETTINGS (Screen 6).
 *
 * Reachable from Home's idle state via DOUBLE_CLICK (see
 * ScreenHome::onInput() — this screen was previously registered but had no
 * actual navigation entry point anywhere in the UI; this is the chosen
 * gesture, picked because Home's IDLE case doesn't already use it, unlike
 * SHORT_PRESS/LONG_PRESS/START_BTN).
 *
 * Currently one real row: "Show QR" (item I) — opens a full-screen QR code
 * for the web dashboard's URL (STA mode), or a Wi-Fi-join QR for the AP
 * fallback network, with a SHORT_PRESS toggle to a second QR for the AP's
 * own URL in that case.
 *
 * TODO when extending this further: Wi-Fi mode (AP/Client toggle), SD card
 * status + Format action (confirm required), Battery status (read-only —
 * no battery ADC is wired in Planned_Pin_Layout.txt yet), Firmware version
 * (read-only), Factory Reset (bottom, red, long-press-to-confirm). Display
 * brightness was considered but explicitly dropped from scope (no LEDC
 * PWM — backlight stays a plain digitalWrite HIGH).
 */
#pragma once
#include "../state_machine.h"
#include "../app_context.h"
#include "../drivers/renderer.h"

class ScreenDeviceSettings : public Screen {
public:
  explicit ScreenDeviceSettings(AppContext &ctx) : _ctx(ctx) {}

  void enter() override {
    _sub = Sub::MENU;
    _qrShowingUrl = false;
    drawMenu();
  }

  void onInput(const InputEvent &ev) override {
    if (_sub == Sub::MENU) {
      if (ev.type == InputType::SHORT_PRESS) {
        _sub = Sub::QR_VIEW;
        _qrShowingUrl = false;
        drawQrView();
      } else if (ev.type == InputType::LONG_PRESS || ev.type == InputType::STOP_BTN) {
        _ctx.stateMachine->changeTo(ScreenId::HOME);
      }
    } else { // Sub::QR_VIEW
      bool apMode = _ctx.wifiIsApMode && _ctx.wifiIsApMode();
      if (ev.type == InputType::SHORT_PRESS && apMode) {
        // Only AP mode has two QRs (join + URL) to toggle between — a
        // no-op in STA mode, where there's only the one dashboard URL.
        _qrShowingUrl = !_qrShowingUrl;
        drawQrView();
      } else if (ev.type == InputType::LONG_PRESS || ev.type == InputType::STOP_BTN) {
        _sub = Sub::MENU;
        drawMenu();
      }
    }
  }

private:
  enum class Sub { MENU, QR_VIEW };

  void drawMenu() {
    _ctx.renderer->clear();
    _ctx.renderer->centeredTitle("SETTINGS", 10, COLOR_WHITE, 1);
    _ctx.renderer->menuRow(3, "Show QR", /*selected=*/true, /*editing=*/false);
    _ctx.renderer->printLine(9, "PRESS: OPEN QR", COLOR_GREY);
    _ctx.renderer->printLine(11, "HOLD: BACK (=HOME)", COLOR_GREY);
  }

  void drawQrView() {
    bool apMode = _ctx.wifiIsApMode && _ctx.wifiIsApMode();
    char text[96];

    // AP mode offers two QRs (join + the AP's own dashboard URL),
    // SHORT_PRESS toggles between them (see onInput()); STA mode only
    // ever has the one dashboard URL. No on-screen caption — the QR is
    // genuinely full-screen (per the user's request), so there's no
    // reliable margin left to put one in once the quiet zone is honoured;
    // the toggle itself is how AP mode's "also reach the URL" requirement
    // is satisfied, not accompanying text.
    if (apMode && !_qrShowingUrl) {
      const char *join = _ctx.apJoinString ? _ctx.apJoinString() : "";
      strncpy(text, join, sizeof(text) - 1);
      text[sizeof(text) - 1] = '\0';
    } else {
      const char *host = _ctx.wifiHostForQr ? _ctx.wifiHostForQr() : "";
      snprintf(text, sizeof(text), "http://%s/", host);
    }

    _ctx.renderer->drawQrCode(text);
  }

  AppContext &_ctx;
  Sub _sub = Sub::MENU;
  bool _qrShowingUrl = false;
};
