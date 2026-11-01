#include "device_settings.h"
#include <Preferences.h>

static const char *kNamespace = "openstat"; // same namespace sweep_params.cpp uses

void loadDeviceSettings(DeviceSettings &d) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  char key[12];
  for (int i = 0; i < kMaxWifiNetworks; i++) {
    snprintf(key, sizeof(key), "wifi%d_ssid", i);
    d.wifi[i].ssid = prefs.getString(key, d.wifi[i].ssid);
    snprintf(key, sizeof(key), "wifi%d_pass", i);
    d.wifi[i].pass = prefs.getString(key, d.wifi[i].pass);
  }
  prefs.end();
}

void saveDeviceSettings(const DeviceSettings &d) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  char key[12];
  for (int i = 0; i < kMaxWifiNetworks; i++) {
    snprintf(key, sizeof(key), "wifi%d_ssid", i);
    prefs.putString(key, d.wifi[i].ssid);
    snprintf(key, sizeof(key), "wifi%d_pass", i);
    prefs.putString(key, d.wifi[i].pass);
  }
  prefs.end();
}
