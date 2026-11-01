/*
 * device_settings.h — small device-level settings persisted to flash (NVS),
 * separate from sweep_params.h's DPV waveform settings. Currently just the
 * saved Wi-Fi network list (item H) — follows sweep_params.cpp's exact
 * Preferences pattern (same "openstat" namespace, open-read-write-close
 * per call).
 *
 * Policy decisions (e.g. "a blank password in a save request means leave
 * the existing one unchanged") live in main.cpp's WS command handler, not
 * here — this module is a plain data+NVS layer, same split as
 * sweep_params.h/SweepParams vs. main.cpp's handleWsMessage().
 */
#pragma once
#include <Arduino.h>

// Item H — confirmed with the user: 4 saved networks, web-dashboard-only
// (no on-device text entry — SSID/password via the rotary encoder's
// character-by-character label-entry pattern would be slow, and 4 covers
// typical use: home + lab + hotspot + spare).
static const int kMaxWifiNetworks = 4;

struct WifiCredential {
  String ssid;  // empty = unused slot
  String pass;
};

struct DeviceSettings {
  WifiCredential wifi[kMaxWifiNetworks];
};

// load keeps d's existing values for any key not stored yet (same
// contract as sweep_params.h's loadSweepParams()).
void loadDeviceSettings(DeviceSettings &d);
void saveDeviceSettings(const DeviceSettings &d);
