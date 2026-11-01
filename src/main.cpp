/*
 * OpenStat — ESP32-S3 potentiostat firmware: web dashboard + onboard GUI
 * -----------------------------------------------------------------------
 * Board:   Waveshare ESP32-S3-DEV-KIT-N32R16V-M
 * Serves:  /data/index.html (adapted potentiostat_dashboard_clinical.html)
 * Link:    WebSocket at ws://<device-ip>/ws for live sweep data + controls
 * Onboard: DFRobot Fermion 1.8" TFT driven through a small screen state
 *          machine (state_machine.h / input.h / screens/*) — encoder +
 *          Start/Stop buttons navigate Boot -> Home -> {Params, Files,
 *          Summary, Settings}.
 *
 * WHAT WORKS RIGHT NOW (no AD5941 needed):
 *   - Wi-Fi station connect, with automatic AP fallback ("OpenStat-Setup")
 *   - Async web server + LittleFS serving the dashboard UI
 *   - WebSocket protocol matching the dashboard's expected JSON messages
 *   - A simulated DPV sweep engine (Gaussian peak + noise, used only when
 *     USE_REAL_AFE is 0) so you can develop/test the whole web UI on the
 *     ESP32 without the AFE wired up. The dashboard's technique selector
 *     was DPV-only even before this — the firmware never implemented
 *     CV/SWV/LSV as distinct sweep types.
 *   - Real SD card storage for sweep results (CSV per sweep + a JSON
 *     index), using the microSD slot built into the Fermion TFT board
 *     (SDCS on GPIO2, sharing the display's SPI bus).
 *   - Real TFT rendering (see drivers/renderer.h) mirroring the
 *     dashboard's live readout, auto-scaling graph, progress bar, and
 *     footer.
 *   - Onboard navigation via rotary encoder + Start/Stop buttons, fully
 *     working for Boot, Home, Params, and Files; Device Settings has one
 *     real row (Show QR — a full-screen QR for the dashboard URL or the
 *     AP's Wi-Fi-join credentials, see screens/screen_settings.h),
 *     reachable from Home via DOUBLE_CLICK; Post-Scan Summary shows which
 *     of complete/stopped/overload/error actually happened but is
 *     otherwise still a stub (no peak/replay/save-row yet).
 *
 * AFE (see drivers/ad5941.h/.cpp): implements a real, polled (no on-chip
 * sequencer) DPV driver — SPI register access, mandatory power-up sequence,
 * LPDAC-generated staircase+pulse waveform, HSTIA/ADC dual-sample
 * (Ibefore/Iend) current capture — built directly from docs/ad5940-5941.pdf
 * with page-cited register values (see the extensive comments in
 * ad5941.h/.cpp for exactly what's a datasheet fact vs. an engineering
 * assumption still needing hardware verification, e.g. the fixed VZERO0
 * bias point and the HSTIA stabilization cap). Flip USE_REAL_AFE to 1 once
 * you've verified those assumptions against your actual board — until then
 * this runs with simulated Gaussian-peak sweep data instead.
 *
 * Required libraries (Arduino IDE Library Manager / PlatformIO):
 *   - ESPAsyncWebServer, AsyncTCP (ESP32Async org forks)
 *   - ArduinoJson (v7)
 *   - Adafruit GFX Library, Adafruit ST7735 and ST7789 Library
 *   - LittleFS, SD (bundled with the ESP32 Arduino core)
 */

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>

#include "sweep_params.h"
#include "device_settings.h"
#include "drivers/ad5941.h"
#include "drivers/renderer.h"
#include "state_machine.h"
#include "input.h"
#include "app_context.h"
#include "screens/screen_boot.h"
#include "screens/screen_home.h"
#include "screens/screen_params.h"
#include "screens/screen_summary.h"
#include "screens/screen_files.h"
#include "screens/screen_settings.h"

// ---------------------------------------------------------------------
// Wi-Fi credentials — fill these in, or leave STA_SSID empty to boot
// straight into AP mode.
// ---------------------------------------------------------------------
static const char *STA_SSID = "Daniel's S24+";               // your home/lab network
static const char *STA_PASS = "poephol1";
static const char *AP_SSID  = "OpenStat-Setup"; // fallback access point
static const char *AP_PASS  = "openstat123";    // must be >= 8 chars
static const unsigned long STA_CONNECT_TIMEOUT_MS = 20000; // phone hotspots can be slow to accept a new device

// ---------------------------------------------------------------------
// Pin map — from Planned_Pin_Layout.txt
// ---------------------------------------------------------------------
// AD5941 (AFE) SPI
#define PIN_AFE_RESET  9
#define PIN_AFE_CS    10
#define PIN_AFE_MOSI  11
#define PIN_AFE_SCLK  12
#define PIN_AFE_MISO  13
#define PIN_AFE_GPIO0 14

// DFRobot Fermion 1.8" TFT (ST7735-class) — SD card shares this bus via SDCS
#define PIN_TFT_DC    1
#define PIN_TFT_SDCS  2
#define PIN_TFT_CS    42
#define PIN_TFT_MISO  41
#define PIN_TFT_MOSI  40
#define PIN_TFT_SCLK  39
#define PIN_TFT_RES   38
#define PIN_TFT_BLK   5

// Rotary encoder + dedicated Start/Stop buttons
#define PIN_ENC_SW  16
#define PIN_ENC_A   15
#define PIN_ENC_B   7
#define PIN_START   17
#define PIN_STOP    18

// Set to 1 once the AD5940-derived sequencer is ported onto ad5941.cpp
#define USE_REAL_AFE 1

// ---------------------------------------------------------------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AD5941 afe(PIN_AFE_CS, PIN_AFE_RESET, PIN_AFE_GPIO0);

// TFT and SD card share one SPI bus (different CS pins) — set up once in
// setup() with displayBus.begin(sclk, miso, mosi, cs), then both the
// renderer and SD.begin() are given a pointer/reference to this same bus.
SPIClass displayBus(HSPI);
Renderer renderer(&displayBus, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RES);
bool sdReady = false;

static const char *RESULTS_DIR = "/results";
static const char *RESULTS_INDEX = "/results/index.json";

// ---------------------------------------------------------------------
// Onboard GUI: state machine, input, and the shared context screens see
// ---------------------------------------------------------------------
InputManager input;
StateMachine stateMachine;
AppContext appCtx;

ScreenBoot screenBoot(appCtx);
ScreenHome screenHome(appCtx);
ScreenParamEntry screenParams(appCtx);
ScreenPostScanSummary screenSummary(appCtx);
ScreenFileBrowser screenFiles(appCtx);
ScreenDeviceSettings screenSettings(appCtx);

// ---------------------------------------------------------------------
// Sweep state
// ---------------------------------------------------------------------
// SweepParams (the DPV waveform) lives in sweep_params.h so the Parameters
// screen can edit it and save it to flash.

struct SweepState {
  bool running = false;
  int  sweepId = 0;
  SweepParams params;
  int  totalPoints = 0;
  int  currentPoint = 0;
  unsigned long startMillis = 0;
  unsigned long lastPointMillis = 0;
  float peakI = 0.0f;
  // Always present (even in USE_REAL_AFE 0 builds, default-valued: 1kOhm/no
  // correction) so sendStatus()/the sweep_start message never need #if
  // guards around reading it.
  AfeRangeResult lastRange;
  bool overloadAborted = false; // set by serviceSweep()'s overload check; consumed by stopSweep()
  // Item B — persisted copies of the most recent stopSweep() call's
  // outcome, read by ScreenPostScanSummary (via AppContext) to show which
  // of "SWEEP COMPLETE" / "STOPPED" / "OVERLOAD" / "ERROR" actually
  // happened, instead of always showing the same hardcoded text regardless
  // of what really occurred. lastStatus mirrors SavedResult::status
  // ("ok"/"overload"/"error") but is NOT itself persisted to SD/NVS —
  // separate field so a plain user-initiated stop (status stays "ok", no
  // dashboard/CSV behavior change) can still be told apart from a true
  // completion on the TFT via lastCompleted.
  String lastStatus = "ok";
  bool lastCompleted = true;
  // Item D/E — calibration data for projecting a graph redraw's cost
  // before attempting it (time scales ~linearly with point count), reset
  // at the start of every sweep. 0/0 means "no measurement yet this
  // sweep" — the first expansion is always attempted rather than
  // projected, since the buffer is still small early on.
  unsigned long lastRedrawCostUs = 0;
  int lastRedrawPointCount = 0;
};

struct SavedResult {
  String file, tech, date, peak, status;
};

SweepState sweep;
// DPV parameters used by Home's Start button. Loaded from flash (NVS) in
// setup(); the Parameters screen edits and saves them.
SweepParams savedParams;
// Item H — saved Wi-Fi networks. Loaded from flash (NVS) in setup(),
// before setupWifi() runs; the web dashboard's Device settings panel
// edits and saves them.
DeviceSettings deviceSettings;

std::vector<SavedResult> savedResults;
// Cached by refreshSdResultsBytesUsed() — see that function's comment for
// why this is cached rather than recomputed on every sendStatus() tick.
uint64_t cachedSdResultsBytes = 0;

// One row per DPV point — CSV columns E_applied_V,I_before_nA,I_end_nA,
// dI_nA,rtia_code,t_ms (item 5). e is the actually-applied E (see
// AD5941::appliedE()), not the requested value.
struct SweepPoint {
  float e, iBefore, iEnd, dI;
  uint8_t rtiaCode;
  unsigned long tMs;
};
std::vector<SweepPoint> sweepBuffer; // for the running sweep

// Toggle states mirrored from the dashboard's right-hand switches
bool toggleTft = true;
bool toggleWsStream = true;
bool toggleAutosave = true;

// Parses the trailing "_NN" number out of a "<label>_NN.csv" filename
// (the format writeSweepCsv()'s caller builds, e.g. "sample_07.csv" -> 7).
// Returns 0 if the filename doesn't end in "_<digits>.csv" — a label that
// itself contains an underscore before non-numeric text safely falls back
// to 0 here rather than being misparsed.
static int extractSweepIdSuffix(const String &filename) {
  int underscore = filename.lastIndexOf('_');
  int dot = filename.lastIndexOf('.');
  if (underscore < 0 || dot < 0 || dot <= underscore + 1) return 0;
  String numPart = filename.substring(underscore + 1, dot);
  for (unsigned i = 0; i < numPart.length(); i++) {
    if (!isDigit(numPart[i])) return 0;
  }
  return numPart.toInt();
}

// Forward declaration — defined further down, grouped with the other SD
// result-file helpers, but called from loadResultsIndex() below.
void refreshSdResultsBytesUsed();

// ---------------------------------------------------------------------
// SD-backed results index (persists the "Saved results" table across reboots)
// ---------------------------------------------------------------------
void loadResultsIndex() {
  savedResults.clear();
  if (!sdReady || !SD.exists(RESULTS_INDEX)) return;
  File f = SD.open(RESULTS_INDEX, FILE_READ);
  if (!f) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  for (JsonObject o : doc["results"].as<JsonArray>()) {
    SavedResult r;
    r.file = String((const char *)(o["file"] | ""));
    r.tech = String((const char *)(o["tech"] | ""));
    r.date = String((const char *)(o["date"] | ""));
    r.peak = String((const char *)(o["peak"] | ""));
    r.status = String((const char *)(o["status"] | "ok"));
    savedResults.push_back(r);
    // Seed sweep.sweepId past every number already used, by ANY label, so
    // startSweep()'s naming loop (further down) doesn't have to re-walk
    // past years of past sessions' numbers one SD.exists() check at a
    // time before finding a free one — see that loop's own comment.
    int id = extractSweepIdSuffix(r.file);
    if (id > sweep.sweepId) sweep.sweepId = id;
  }
  refreshSdResultsBytesUsed(); // see that function's comment for why this
                                // is cached here rather than recomputed
                                // every sendStatus() tick
}

void saveResultsIndex() {
  if (!sdReady) return;
  JsonDocument doc;
  JsonArray arr = doc["results"].to<JsonArray>();
  for (auto &r : savedResults) {
    JsonObject o = arr.add<JsonObject>();
    o["file"] = r.file;
    o["tech"] = r.tech;
    o["date"] = r.date;
    o["peak"] = r.peak;
    o["status"] = r.status;
  }
  File f = SD.open(RESULTS_INDEX, FILE_WRITE);
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void writeSweepCsv(const String &filename) {
  if (!sdReady) return;
  if (!SD.exists(RESULTS_DIR)) SD.mkdir(RESULTS_DIR);
  File f = SD.open(String(RESULTS_DIR) + "/" + filename, FILE_WRITE);
  if (!f) {
    Serial.println("Failed to open result file for writing on SD.");
    return;
  }
  f.println("E_applied_V,I_before_nA,I_end_nA,dI_nA,rtia_code,t_ms");
  for (auto &pt : sweepBuffer) {
    f.printf("%.5f,%.3f,%.3f,%.3f,%u,%lu\n", pt.e, pt.iBefore, pt.iEnd, pt.dI,
             (unsigned)pt.rtiaCode, pt.tMs);
  }
  f.close();
}

// Rough used-space estimate: sums file sizes under /results rather than
// relying on filesystem-level usedBytes(), which isn't reliably available
// for FAT via the Arduino SD library. This opens and sizes every file
// under /results, so its cost scales with how many results are saved —
// too expensive to call on every sendStatus() tick (every 2s while idle),
// which is what it used to do. Instead this updates cachedSdResultsBytes
// and is only called when the result set actually changes (boot, after a
// sweep autosaves, after delete_result, after erase_results); sendStatus()
// just reads the cached value.
void refreshSdResultsBytesUsed() {
  cachedSdResultsBytes = 0;
  if (!sdReady) return;
  File dir = SD.open(RESULTS_DIR);
  if (!dir) return;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) cachedSdResultsBytes += entry.size();
    entry = dir.openNextFile();
  }
  dir.close();
}

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
void broadcastJson(JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

// ---------------------------------------------------------------------
// AppContext callbacks — thin wrappers so screens never touch the
// globals above directly (see app_context.h).
// ---------------------------------------------------------------------
bool ctxWifiUp() { return WiFi.getMode() == WIFI_AP || WiFi.status() == WL_CONNECTED; }
const char *ctxIpAddress() {
  static char buf[32];
  if (WiFi.getMode() == WIFI_AP) {
    snprintf(buf, sizeof(buf), "AP %s", WiFi.softAPIP().toString().c_str());
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "IP %s", WiFi.localIP().toString().c_str());
  } else {
    snprintf(buf, sizeof(buf), "No Wi-Fi");
  }
  return buf;
}
// Item I — see app_context.h's comment on these three.
bool ctxWifiIsApMode() { return WiFi.getMode() == WIFI_AP; }
const char *ctxWifiHostForQr() {
  static char buf[32];
  if (WiFi.getMode() == WIFI_AP) {
    snprintf(buf, sizeof(buf), "%s", WiFi.softAPIP().toString().c_str());
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "%s", WiFi.localIP().toString().c_str());
  } else {
    buf[0] = '\0';
  }
  return buf;
}
const char *ctxApJoinString() {
  static char buf[96];
  snprintf(buf, sizeof(buf), "WIFI:S:%s;T:WPA;P:%s;;", AP_SSID, AP_PASS);
  return buf;
}
// TFT-safe (ASCII-only) RTIA text — "R" for ohms rather than "Ω", since the
// TFT's built-in GFX font can't render it (unlike sendStatus()'s web-facing
// tiaGain field, which uses the real glyph and is fine in-browser).
const char *ctxRtiaText() {
  static char buf[12];
  float ohms = afe.currentRtiaOhms();
  if (ohms >= 1000.0f) snprintf(buf, sizeof(buf), "%.0fkR", ohms / 1000.0f);
  else snprintf(buf, sizeof(buf), "%.0fR", ohms);
  return buf;
}
bool ctxSdReady() { return sdReady; }
int ctxBatteryPct() { return 100; } // no battery ADC wired yet — see screen_settings.h TODO
bool ctxIsSweepRunning() { return sweep.running; }
int ctxSweepProgressPct() {
  return sweep.totalPoints > 0 ? (sweep.currentPoint * 100 / sweep.totalPoints) : 0;
}
unsigned long ctxSweepElapsedSec() {
  return sweep.running ? (millis() - sweep.startMillis) / 1000 : 0;
}
const char *ctxCurrentFilename() {
  static char buf[64];
  snprintf(buf, sizeof(buf), "%s_%02d.csv", sweep.params.label.c_str(), sweep.sweepId);
  return buf;
}
const char *ctxCurrentTechnique() {
  static char buf[8];
  strncpy(buf, sweep.params.technique.c_str(), sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}
// Item B — see sweep.lastStatus/lastCompleted's comment (SweepState).
const char *ctxLastSweepStatus() {
  static char buf[12];
  strncpy(buf, sweep.lastStatus.c_str(), sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}
bool ctxLastSweepCompleted() { return sweep.lastCompleted; }
int ctxSavedResultsCount() { return (int)savedResults.size(); }

bool ctxSavedResultAt(int idx, char *file, size_t fileLen, char *peak, size_t peakLen) {
  if (idx < 0 || idx >= (int)savedResults.size()) return false;
  snprintf(file, fileLen, "%s", savedResults[idx].file.c_str());
  snprintf(peak, peakLen, "%s", savedResults[idx].peak.c_str());
  return true;
}

// Streams a saved sweep's CSV ("voltage_V,current_nA" rows, see
// writeSweepCsv()) to the visitor one row at a time. Lines that are the
// header, blank, '#' comments or otherwise unparseable are skipped.
bool ctxForEachResultPoint(const char *file, PointVisitor visit, void *user) {
  if (!sdReady) return false;
  File f = SD.open(String(RESULTS_DIR) + "/" + file, FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    char c = line[0];
    if (!(c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9'))) continue; // header row
    // Columns: E_applied_V,I_before_nA,I_end_nA,dI_nA,rtia_code,t_ms — the
    // plot only needs E (col 0) and dI (col 3); split on ALL commas rather
    // than just the first, so a naive first-comma split doesn't silently
    // hand back I_before_nA instead of dI_nA.
    int c0 = line.indexOf(',');
    if (c0 <= 0) continue;
    int c1 = line.indexOf(',', c0 + 1);
    if (c1 < 0) continue;
    int c2 = line.indexOf(',', c1 + 1);
    if (c2 < 0) continue;
    int c3 = line.indexOf(',', c2 + 1);
    if (c3 < 0) continue;
    float e = line.substring(0, c0).toFloat();
    float dI = line.substring(c2 + 1, c3).toFloat();
    visit(e, dI, user);
  }
  f.close();
  return true;
}

// Forward declarations for the sweep engine functions AppContext points at
bool startSweep(const SweepParams &p);
void stopSweep(bool completed);

void sendStatus() {
  JsonDocument doc;
  doc["type"] = "status";
  doc["flashUsedKB"] = (LittleFS.usedBytes()) / 1024;
  doc["flashTotalKB"] = (LittleFS.totalBytes()) / 1024;
  doc["sdReady"] = sdReady;
  doc["sdUsedKB"] = cachedSdResultsBytes / 1024;
  doc["sdTotalKB"] = sdReady ? (SD.cardSize() / 1024) : 0;
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  // Live RTIA (auto-ranged per sweep, see AD5941::autoRangeRtia()) rather
  // than the old hardcoded "1 MΩ" placeholder. currentRtiaOhms() is a plain
  // member-data read (no SPI), safe to call every 2s tick even before any
  // sweep has run (reset()'s 1kOhm default) or with USE_REAL_AFE 0.
  float rtiaOhms = afe.currentRtiaOhms();
  char tiaBuf[24];
  if (rtiaOhms >= 1000.0f) snprintf(tiaBuf, sizeof(tiaBuf), "%.0f k\xCE\xA9", rtiaOhms / 1000.0f);
  else snprintf(tiaBuf, sizeof(tiaBuf), "%.0f \xCE\xA9", rtiaOhms);
  doc["tiaGain"] = tiaBuf;
  doc["adcRange"] = "\xC2\xB1" "0.9 V"; // AD5941::kAdcInputWindowVolts — fixed, doesn't vary with RTIA (old "±2.4V" was never right either)
  doc["rtiaOhms"] = rtiaOhms;
  doc["rtiaAtMaxRange"] = sweep.lastRange.atMaxRange;
  doc["rangeCorrection"] = sweep.lastRange.correction;
  uint32_t heapTotal = ESP.getHeapSize();
  uint32_t heapFree  = ESP.getFreeHeap();
  uint32_t heapUsed  = heapTotal - heapFree;
  doc["heapUsedKB"] = heapUsed / 1024;
  doc["heapTotalKB"] = heapTotal / 1024;
  doc["heapPct"] = heapTotal ? (heapUsed * 100 / heapTotal) : 0;
  broadcastJson(doc);

  // Chrome (top status icons) only needs a refresh here while idle on
  // Home — during a sweep, serviceSweep()'s per-point renderer calls
  // already redraw it via sweepPoint()/sweepDone().
  if (toggleTft && stateMachine.currentId() == ScreenId::HOME && !sweep.running) {
    bool wifiUp = ctxWifiUp();
    const char *rtiaTxt = ctxRtiaText();
    renderer.setStatus(wifiUp, sdReady, 100, rtiaTxt);
    renderer.drawChrome(sweep.params.technique.c_str(), wifiUp, sdReady, 100, rtiaTxt);
  }
}

// Simple deterministic-ish "voltammogram" so the dashboard has something
// real to plot before the AFE driver is finished. Replace with AFE FIFO
// reads when USE_REAL_AFE is set.
float simulateCurrentNa(float v, float peakCenter) {
  float width = 0.045f;
  float height = 180.0f;
  float gauss = height * expf(-((v - peakCenter) * (v - peakCenter)) / (2 * width * width));
  float baseline = v * 8.0f;
  float noise = (random(-100, 100) / 100.0f) * 4.0f;
  return gauss + baseline + noise;
}

// True if a result with this file name is already in the results index or on
// the SD card. loadResultsIndex() seeds sweep.sweepId past every number
// already in the index at boot, so the do-while loop below normally only
// needs one try — this check remains as a safety net for the rare case of
// a file on SD that isn't in the index (e.g. a corrupted/lost index.json),
// which the seeding can't account for.
static bool resultNameTaken(const char *name) {
  for (auto &r : savedResults) {
    if (r.file == name) return true;
  }
  return sdReady && SD.exists(String(RESULTS_DIR) + "/" + name);
}

// Item A — plain, non-capturing wrapper so it can be passed as the C
// function pointer AD5941::autoRangeRtia()'s stopRequested parameter
// expects (matching this project's existing AppContext callback
// convention of plain function pointers, not std::function).
static bool checkStopRequested() { return input.isStopRequested(); }

// Item B — returns true only once the sweep has actually started running
// (reached afe.startSweep()/the simulated-path equivalent below); false for
// every bail-out reason (already running, STOP pressed mid-probe,
// configureSweep() refusing the timing). Callers use this to avoid
// optimistically showing "sweeping" state for a sweep that never began —
// see ScreenHome::onInput()'s START_BTN handling.
bool startSweep(const SweepParams &p) {
  if (sweep.running) {
    // Already mid-sweep (AFE busy) — ignore. Both the onboard Start
    // button and the web dashboard's "start" command funnel through here,
    // so this is the one place that has to enforce "only if idle".
    Serial.println("Sweep state: START ignored (already RUNNING)");
    return false;
  }

  // Item A — discard any stale STOP latched from before this sweep began
  // (e.g. STOP used moments earlier as "back" in a menu) so it can't be
  // mistaken for a request to abort the sweep that's only just starting.
  input.clearStopRequest();

  sweep.params = p;
  sweep.running = true;
  char candidate[64];
  do {
    sweep.sweepId++;
    snprintf(candidate, sizeof(candidate), "%s_%02d.csv", p.label.c_str(), sweep.sweepId);
  } while (resultNameTaken(candidate)); // keep counting until the name is free
  sweep.currentPoint = 0;
  sweep.peakI = 0.0f;
  sweep.overloadAborted = false;
  sweep.startMillis = millis();
  sweep.lastPointMillis = 0;
  sweepBuffer.clear();

  float stepV = p.stepMv / 1000.0f;
  // fabsf: this is only a preliminary capacity hint for sweepBuffer.reserve()
  // below — afe.configureSweep() computes the authoritative totalPoints
  // right after (overwriting this), but fabsf keeps this estimate correct
  // too for a descending sweep (endV < startV) rather than defaulting to 1.
  sweep.totalPoints = stepV > 0 ? (int)(fabsf(p.endV - p.startV) / stepV) : 0;
  if (sweep.totalPoints <= 0) sweep.totalPoints = 1;
  sweepBuffer.reserve(sweep.totalPoints);

#if USE_REAL_AFE
  if (p.autoRange) {
    // Auto-range RTIA before every sweep — safety-first (starts at 1kOhm,
    // only ever steps up) and picks the range with the best resolution for
    // whatever's actually connected, via a probe-and-ratiometric-self-check
    // pass (see AD5941::autoRangeRtia()'s doc comment for the full algorithm
    // and why this isn't a true absolute calibration).
    const float probeVolts[3] = { p.startV, (p.startV + p.endV) * 0.5f, p.endV };
    sweep.lastRange = afe.autoRangeRtia(probeVolts, 3, checkStopRequested);
    if (sweep.lastRange.aborted) {
      // Item A — STOP was pressed mid-probe (this is the ~multi-second
      // blocking stretch where a press could otherwise be lost entirely).
      // Same bail-out shape as a configureSweep() failure below: reset and
      // refuse to start rather than continuing into a sweep nobody asked
      // to begin.
      Serial.println("Sweep state: START aborted (stop requested during RTIA auto-range)");
      sweep.running = false;
      return false;
    }
    Serial.printf("Auto-range: %.0f ohm (code %d)%s, correction=%.4f, peak probe=%.1f nA\n",
                  sweep.lastRange.rtiaOhms, sweep.lastRange.rtiaCode,
                  sweep.lastRange.atMaxRange ? " [AT MAX RANGE — still underscale]" : "",
                  sweep.lastRange.correction, sweep.lastRange.peakProbeNa);
  } else {
    // Fixed range (confirmed with the user) — skips the probing pass
    // entirely; sweep.lastRange.correction stays 1.0 (diagnostic-only
    // field, not consulted by AD5941::applyCalibration() — see
    // kGainTable/kOffsetTable_nA in ad5941.h for the calibration that
    // actually applies).
    afe.useFixedRtia(p.fixedRtiaCode);
    sweep.lastRange = AfeRangeResult();
    sweep.lastRange.rtiaCode = p.fixedRtiaCode;
    sweep.lastRange.rtiaOhms = afe.currentRtiaOhms();
    Serial.printf("Fixed RTIA: %.0f ohm (code %d) — auto-ranging skipped\n",
                  sweep.lastRange.rtiaOhms, sweep.lastRange.rtiaCode);
  }

  if (!afe.configureSweep(p.startV, p.endV, stepV, p.pulseAmpMv / 1000.0f, p.pulseWidthMs, p.intervalMs,
                           p.equilibrationMs)) {
    // configureSweep() already printed exactly why (timing too tight for
    // the ADC averaging window) — refuse to start rather than silently
    // changing the requested pulse width/interval (explicit user decision).
    Serial.println("Sweep state: START aborted (see above)");
    sweep.running = false;
    return false;
  }
  sweep.totalPoints = afe.totalSteps(); // AD5941 computes this itself — keep in sync
  afe.startSweep();
#endif

  // Item D/E — seed the TFT graph's Y-axis range. Use the auto-range
  // probe's peak estimate (with margin, spanning both polarities since DPV
  // current can go either sign) when one is genuinely available; otherwise
  // fall back to the same default the graph always used before this
  // feature (fixed-RTIA sweeps have no probe to seed from, and so does the
  // USE_REAL_AFE=0 simulated/demo path — unaffected by this change). Either
  // way this is only a starting point: serviceSweep() expands it further if
  // a reading still falls outside it.
  sweep.lastRedrawCostUs = 0;
  sweep.lastRedrawPointCount = 0;
  float graphMinNa = GRAPH_I_MIN_NA, graphMaxNa = GRAPH_I_MAX_NA;
  if (p.autoRange && sweep.lastRange.peakProbeNa > 0.0f) {
    float peak = max(sweep.lastRange.peakProbeNa, 1.0f) * 1.3f;
    graphMinNa = -peak;
    graphMaxNa = peak;
  }
  if (toggleTft && stateMachine.currentId() == ScreenId::HOME) {
    renderer.sweepStart(p.technique.c_str(), graphMinNa, graphMaxNa);
  }

  Serial.printf("Sweep state: RUNNING (%s, %d points, %s..%sV)\n", p.technique.c_str(),
                sweep.totalPoints, String(p.startV, 3).c_str(), String(p.endV, 3).c_str());

  JsonDocument doc;
  doc["type"] = "sweep_start";
  doc["sweepId"] = sweep.sweepId;
  doc["label"] = p.label;
  doc["technique"] = p.technique;
  doc["totalPoints"] = sweep.totalPoints;
  doc["rtiaOhms"] = sweep.lastRange.rtiaOhms;
  doc["rangeCorrection"] = sweep.lastRange.correction;
  doc["rtiaAtMaxRange"] = sweep.lastRange.atMaxRange;
  broadcastJson(doc);
  return true;
}

void stopSweep(bool completed) {
  if (!sweep.running) {
    // Already idle — avoid double-appending to savedResults/CSV if
    // stop is requested twice in a row (e.g. STOP_BTN pressed while a
    // "stop" WS command from the dashboard is still in flight).
    Serial.println("Sweep state: STOP ignored (already IDLE)");
    return;
  }
  sweep.running = false;

#if USE_REAL_AFE
  afe.stopSweep(); // halts the sequencer — safe to call whether the sweep completed or was aborted
#endif

  Serial.printf("Sweep state: IDLE (%s, %d/%d points captured)\n",
                completed ? "completed" : "aborted", sweep.currentPoint, sweep.totalPoints);

  char filename[64];
  snprintf(filename, sizeof(filename), "%s_%02d.csv", sweep.params.label.c_str(), sweep.sweepId);
  char peakBuf[24];
  snprintf(peakBuf, sizeof(peakBuf), "%.1f nA", sweep.peakI);

  if (toggleAutosave) {
    writeSweepCsv(filename);
    refreshSdResultsBytesUsed();
  }

  SavedResult r;
  r.file = filename;
  r.tech = sweep.params.technique;
  r.date = "now"; // wire up an RTC/NTP timestamp here
  r.peak = peakBuf;
  // Item B — precedence: overload > error (zero points, no overload) > ok.
  // The zero-points-but-not-overload case isn't reachable through any
  // path today (the only current zero-point stopSweep() call is the
  // overload one), but this guards against ever silently recording a
  // genuinely empty run as "ok" if that ever changes.
  if (sweep.overloadAborted) r.status = "overload";
  else if (sweep.currentPoint == 0) r.status = "error";
  else r.status = "ok";
  sweep.lastStatus = r.status;
  sweep.lastCompleted = completed;
  savedResults.insert(savedResults.begin(), r);
  saveResultsIndex();

  if (toggleTft && stateMachine.currentId() == ScreenId::HOME) {
    // Item D/E — final correctness pass: a mid-sweep expansion can have
    // been deferred (tight interval, no slack) and never caught up before
    // the sweep ended (e.g. an early user-initiated stop, not the normal
    // last-point-forces-it path in serviceSweep()). Not time-constrained
    // here — the sweep is over — so just check the whole buffer once and
    // redraw if anything was ever left outside the currently-applied
    // range, rather than leaving the final trace visibly clipped.
    float dataMin = 0.0f, dataMax = 0.0f;
    bool haveData = false;
    for (auto &pt : sweepBuffer) {
      if (!haveData) { dataMin = dataMax = pt.dI; haveData = true; }
      else {
        if (pt.dI < dataMin) dataMin = pt.dI;
        if (pt.dI > dataMax) dataMax = pt.dI;
      }
    }
    if (haveData && (renderer.graphNeedsExpand(dataMin) || renderer.graphNeedsExpand(dataMax))) {
      float newMin, newMax;
      renderer.computeExpandedRange(dataMin, newMin, newMax);
      if (renderer.graphNeedsExpand(dataMax)) {
        float newMin2, newMax2;
        renderer.computeExpandedRange(dataMax, newMin2, newMax2);
        newMin = min(newMin, newMin2);
        newMax = max(newMax, newMax2);
      }
      renderer.applyGraphRange(newMin, newMax);
      for (auto &pt : sweepBuffer) renderer.drawTraceLine(pt.e, pt.dI, sweep.params.startV, sweep.params.endV);
    }
    renderer.sweepDone();
  }

  JsonDocument doc;
  doc["type"] = "sweep_done";
  doc["completed"] = completed;
  JsonObject result = doc["result"].to<JsonObject>();
  result["file"] = r.file;
  result["tech"] = r.tech;
  result["date"] = r.date;
  result["peak"] = r.peak;
  result["status"] = r.status;
  broadcastJson(doc);
}

void serviceSweep() {
  if (!sweep.running) return;
  unsigned long now = millis();

  float v, i_nA;
  float iBefore, iEnd;
  uint8_t rtiaCode;
#if USE_REAL_AFE
  // afe.poll() runs its own base-settle -> sample -> pulse -> sample state
  // machine internally (see ad5941.cpp) and only returns true once a full
  // DPV point (deltaI = Iend - Ibefore) is ready — this call is cheap and
  // safe to make every loop() tick, unlike the simulated path below it
  // doesn't need its own pacing gate here.
  if (!afe.poll(v, i_nA)) return;
  if (afe.lastSampleOverloaded()) {
    // Abort immediately (confirmed with the user) rather than flag-and-
    // continue — a step past 80% of full-scale for the active RTIA range
    // isn't trustworthy. The overloading sample itself is discarded (not
    // pushed to sweepBuffer/CSV); everything captured before it still
    // flushes via stopSweep()'s existing partial-data path, untouched.
    Serial.println("Sweep aborted: sample exceeded 80% of full-scale for the active RTIA range.");
    sweep.overloadAborted = true;
    stopSweep(false);
    return;
  }
  iBefore = afe.lastIBeforeNa();
  iEnd = afe.lastIEndNa();
  rtiaCode = afe.currentRtiaCode();
#else
  // pace simulated points roughly by pulse width + interval so the UI feels realistic
  unsigned long pointIntervalMs = (unsigned long)max(20.0f, sweep.params.pulseWidthMs);
  if (now - sweep.lastPointMillis < pointIntervalMs) return;
  sweep.lastPointMillis = now;

  float stepV = sweep.params.stepMv / 1000.0f;
  v = sweep.params.startV + stepV * sweep.currentPoint;
  float peakCenter = sweep.params.startV + (sweep.params.endV - sweep.params.startV) * 0.55f;
  i_nA = simulateCurrentNa(v, peakCenter);
  iBefore = 0.0f; // no separate Ibefore/Iend model in the simulated path
  iEnd = i_nA;
  rtiaCode = 0;
#endif

  if (i_nA > sweep.peakI) sweep.peakI = i_nA;
  sweep.currentPoint++;
  unsigned long tMs = now - sweep.startMillis;
  sweepBuffer.push_back({v, iBefore, iEnd, i_nA, rtiaCode, tMs});

  int progressPct = (sweep.totalPoints > 0) ? (sweep.currentPoint * 100 / sweep.totalPoints) : 0;

  // Only draw to the physical screen while Home is actually the active
  // screen — if the user has navigated elsewhere, the sweep keeps running
  // (and streaming over WebSocket / logging to SD) but doesn't fight that
  // screen for the display.
  if (toggleTft && stateMachine.currentId() == ScreenId::HOME) {
    // Item D/E — expand the graph's Y-axis range before drawing this
    // point, if it would otherwise clip. Re-checked fresh every tick
    // against whatever range is CURRENTLY applied (not a separately
    // tracked "pending" target) — if a deferral below leaves the range
    // too small, the very next out-of-range point re-triggers this same
    // check and computes a (correspondingly bigger) candidate again.
    if (renderer.graphNeedsExpand(i_nA)) {
      float newMin, newMax;
      renderer.computeExpandedRange(i_nA, newMin, newMax);

      // Projected cost of replaying the whole buffer at the new scale,
      // from the last redraw actually measured THIS sweep (cost scales
      // ~linearly with point count) — 0 the first time, which always
      // proceeds since the buffer is still small early in a sweep.
      size_t n = sweepBuffer.size();
      unsigned long projectedUs = 0;
      if (sweep.lastRedrawPointCount > 0) {
        projectedUs = (unsigned long)((uint64_t)sweep.lastRedrawCostUs * n /
                                       (uint64_t)sweep.lastRedrawPointCount);
      }
      // Budget: at most half the interval, minus the ~40ms the AD5941's
      // two per-step sampling windows always take regardless — never eat
      // into THAT (measurement takes priority, confirmed with the user).
      // A sweep that's about to stop anyway (this is its last point)
      // redraws unconditionally so the final on-screen trace is never
      // left clipped.
      bool sweepEnding = (sweep.currentPoint >= sweep.totalPoints);
      long budgetUs = (long)(sweep.params.intervalMs * 500.0f) - 40000L;
      if (sweepEnding || budgetUs <= 0 || projectedUs <= (unsigned long)budgetUs) {
        unsigned long t0 = micros();
        renderer.applyGraphRange(newMin, newMax);
        for (auto &pt : sweepBuffer) {
          renderer.drawTraceLine(pt.e, pt.dI, sweep.params.startV, sweep.params.endV);
        }
        sweep.lastRedrawCostUs = micros() - t0;
        sweep.lastRedrawPointCount = (int)n;
        if (projectedUs > (unsigned long)max(0L, budgetUs)) {
          Serial.printf("Graph redraw: %u points in %luus (over budget %ldus, ran anyway%s)\n",
                        (unsigned)n, sweep.lastRedrawCostUs, budgetUs,
                        sweepEnding ? " - sweep ending" : "");
        }
      } else {
        Serial.printf("Graph redraw: deferred (%u points, projected %luus > budget %ldus)\n",
                      (unsigned)n, projectedUs, budgetUs);
      }
    }
    renderer.sweepPoint(v, i_nA, sweep.params.startV, sweep.params.endV, progressPct,
                         ctxCurrentFilename(), (now - sweep.startMillis) / 1000);
  }

  if (toggleWsStream) {
    JsonDocument doc;
    doc["type"] = "sweep_point";
    doc["v"] = v;             // == e_applied_V — kept as "v" for the existing dashboard JS
    doc["i"] = i_nA;          // == dI_nA — kept as "i" for the existing dashboard JS
    doc["iBefore"] = iBefore;
    doc["iEnd"] = iEnd;
    doc["rtiaCode"] = rtiaCode;
    doc["tMs"] = tMs;
    doc["elapsedMs"] = now - sweep.startMillis;
    doc["progressPct"] = progressPct;
    broadcastJson(doc);
  }

  if (sweep.currentPoint >= sweep.totalPoints) {
    stopSweep(true);
  }
}

// Builds and sends the current savedResults as a "results_list" message —
// shared by list_results, erase_results and delete_result so the client's
// table is always refreshed from the same, single source of truth.
void broadcastResultsList() {
  JsonDocument out;
  out["type"] = "results_list";
  JsonArray arr = out["results"].to<JsonArray>();
  for (auto &r : savedResults) {
    JsonObject o = arr.add<JsonObject>();
    o["file"] = r.file;
    o["tech"] = r.tech;
    o["date"] = r.date;
    o["peak"] = r.peak;
    o["status"] = r.status;
  }
  broadcastJson(out);
}

// ---------------------------------------------------------------------
// WebSocket command handling
// ---------------------------------------------------------------------
void handleWsMessage(const String &msg) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) return;

  const char *cmd = doc["cmd"] | "";

  if (strcmp(cmd, "start") == 0) {
    SweepParams p;
    p.technique     = String((const char *)(doc["technique"] | "DPV"));
    p.startV        = doc["startV"] | 0.80f;
    p.endV          = doc["endV"] | 1.30f;
    p.stepMv        = doc["stepMv"] | 5.0f;
    p.pulseAmpMv    = doc["pulseAmpMv"] | 25.0f;
    p.pulseWidthMs  = doc["pulseWidthMs"] | 50.0f;
    p.intervalMs    = doc["intervalMs"] | 500.0f;
    p.equilibrationMs = doc["equilibrationMs"] | 10000.0f; // matches SweepParams' own default
    p.label         = String((const char *)(doc["label"] | "sample"));
    p.autoRange     = doc["autoRange"] | true; // matches SweepParams' own default
    p.fixedRtiaCode = (uint8_t)(doc["fixedRtiaCode"] | 3);
    // Item B — unlike the TFT Parameters screen's save path and the serial
    // setrtia command, this handler previously built SweepParams straight
    // from JSON and passed it to startSweep() with no centralized clamp,
    // relying on each downstream consumer's own defensive bounds-check
    // instead. Matches the TFT path now.
    sanitizeSweepParams(p);
    savedParams = p; // web-started sweeps also update "last used" params
    startSweep(p);

  } else if (strcmp(cmd, "stop") == 0) {
    stopSweep(false);

  } else if (strcmp(cmd, "save_preset") == 0) {
    // TODO: persist named presets to LittleFS as JSON.

  } else if (strcmp(cmd, "toggle") == 0) {
    String key = String((const char *)(doc["key"] | ""));
    bool on = doc["on"] | false;
    if (key == "tft") toggleTft = on;
    else if (key == "ws_stream") toggleWsStream = on;
    else if (key == "autosave") toggleAutosave = on;

  } else if (strcmp(cmd, "list_results") == 0) {
    broadcastResultsList();

  } else if (strcmp(cmd, "delete_result") == 0) {
    // Reject anything that isn't a plain filename — file flows straight
    // into an SD path below, and this is a destructive operation (unlike
    // the pre-existing /download route, which is read-only), so it's
    // worth guarding against a malformed/malicious file param escaping
    // RESULTS_DIR via a path separator or "..".
    String file = String((const char *)(doc["file"] | ""));
    bool validName = file.length() > 0 && file.indexOf('/') < 0 &&
                      file.indexOf('\\') < 0 && file.indexOf("..") < 0;
    if (validName) {
      for (size_t i = 0; i < savedResults.size(); i++) {
        if (savedResults[i].file == file) {
          savedResults.erase(savedResults.begin() + i);
          if (sdReady) {
            SD.remove(String(RESULTS_DIR) + "/" + file);
            saveResultsIndex();
            refreshSdResultsBytesUsed();
          }
          break;
        }
      }
    }
    broadcastResultsList();

  } else if (strcmp(cmd, "erase_results") == 0) {
    savedResults.clear();
    if (sdReady) {
      File dir = SD.open(RESULTS_DIR);
      if (dir) {
        File entry = dir.openNextFile();
        while (entry) {
          String path = String(RESULTS_DIR) + "/" + entry.name();
          bool isDir = entry.isDirectory();
          entry.close();
          if (!isDir) SD.remove(path);
          entry = dir.openNextFile();
        }
        dir.close();
      }
      SD.remove(RESULTS_INDEX);
    }
    cachedSdResultsBytes = 0; // everything under RESULTS_DIR is gone — cheaper than rescanning
    broadcastResultsList();

  } else if (strcmp(cmd, "list_wifi") == 0) {
    // Item H — SSIDs only, never the stored passwords (confirmed with the
    // user). Broadcast like list_results' results_list — fine, since SSIDs
    // aren't sensitive to another dashboard client on the same LAN.
    JsonDocument out;
    out["type"] = "wifi_list";
    JsonArray arr = out["ssids"].to<JsonArray>();
    for (int i = 0; i < kMaxWifiNetworks; i++) arr.add(deviceSettings.wifi[i].ssid);
    broadcastJson(out);

  } else if (strcmp(cmd, "save_wifi") == 0) {
    // Item H — a blank password means "leave that slot's stored password
    // unchanged" (only overwrite when a non-empty value is submitted); a
    // blank SSID *and* password together clears/removes that slot. Saved
    // to NVS immediately but only applied on next boot (restartBtn is the
    // existing "apply now" action) — deliberately not a live reconnect,
    // which would tear down the very WS connection this command arrived
    // on.
    JsonArray networks = doc["networks"].as<JsonArray>();
    int i = 0;
    for (JsonObject net : networks) {
      if (i >= kMaxWifiNetworks) break;
      String ssid = String((const char *)(net["ssid"] | ""));
      String pass = String((const char *)(net["pass"] | ""));
      if (ssid.length() == 0 && pass.length() == 0) {
        deviceSettings.wifi[i].ssid = "";
        deviceSettings.wifi[i].pass = "";
      } else {
        deviceSettings.wifi[i].ssid = ssid;
        if (pass.length() > 0) deviceSettings.wifi[i].pass = pass;
      }
      i++;
    }
    saveDeviceSettings(deviceSettings);
    Serial.println("Wi-Fi networks saved to flash — restart the device to apply.");

  } else if (strcmp(cmd, "restart") == 0) {
    ESP.restart();
  }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WS client #%u connected\n", client->id());
    sendStatus();
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WS client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      String msg((char *)data, len);
      handleWsMessage(msg);
    }
  }
}

// ---------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------
static const char *wifiFailureReason(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:   return "SSID not found (name mismatch, or the hotspot is 5GHz-only)";
    case WL_CONNECT_FAILED:  return "connect failed (wrong password, or security the ESP32 can't use, e.g. WPA3-only)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:    return "never associated (wrong password, weak signal, or the AP rejected it)";
    default:                 return "unknown";
  }
}

// After a failed connect: print why, and which 2.4GHz networks the ESP32 can
// actually see, so a wrong/invisible SSID (5GHz band, curly apostrophe, ...)
// shows up straight away. Entries with non-ASCII characters are flagged, since
// a typographic ’ vs a plain ' is invisible in most terminals.
static void diagnoseWifiFailure() {
  Serial.printf("Wi-Fi: '%s' failed — %s\n", STA_SSID, wifiFailureReason(WiFi.status()));
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  Serial.printf("Wi-Fi: %d 2.4GHz network(s) visible:\n", n);
  bool found = false;
  for (int i = 0; i < n && i < 20; i++) {
    String ssid = WiFi.SSID(i);
    bool match = (ssid == STA_SSID);
    found = found || match;
    bool nonAscii = false;
    for (size_t k = 0; k < ssid.length(); k++) {
      if ((uint8_t)ssid[k] >= 0x80) nonAscii = true;
    }
    Serial.printf("  %s'%s'  ch%d  %ddBm%s\n", match ? "* " : "  ", ssid.c_str(), (int)WiFi.channel(i),
                  (int)WiFi.RSSI(i), nonAscii ? "  [contains non-ASCII characters]" : "");
  }
  WiFi.scanDelete();
  if (!found) {
    Serial.println("Wi-Fi: the configured SSID is NOT in that list. If your hotspot isn't listed at all it is "
                   "on 5GHz (switch it to 2.4GHz); if a similar name is listed, copy that exact spelling.");
  }
}

// Blocks for up to STA_CONNECT_TIMEOUT_MS waiting for the connect attempt
// already started by WiFi.begin() to resolve; mirrors progress as both a
// Serial dot-trail and a TFT line (renderer.begin() has already run by the
// time this executes, see setup()'s ordering). Shared by every WiFi.begin()
// call site in setupWifi() below so they all behave identically.
static bool waitForStaConnect() {
  unsigned long start = millis();
  String dots;
  while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
    if (toggleTft) {
      if (dots.length() < 16) dots += '.';
      renderer.printLine(6, (String("Connecting") + dots).c_str(), COLOR_GREY);
    }
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void setupWifi() {
  WiFi.mode(WIFI_STA);

  // Item H — scan once, then only attempt saved networks that are
  // actually visible right now, strongest signal first. Skips wasting a
  // full STA_CONNECT_TIMEOUT_MS on a saved-but-out-of-range network
  // (which the old single-hardcoded-SSID path never had to worry about,
  // having only ever had one candidate).
  struct Candidate { const char *ssid; const char *pass; int32_t rssi; };
  Candidate candidates[kMaxWifiNetworks];
  int numCandidates = 0;

  int n = WiFi.scanNetworks();
  Serial.printf("Wi-Fi: scanning... %d network(s) visible\n", max(n, 0));
  for (int slot = 0; slot < kMaxWifiNetworks; slot++) {
    if (deviceSettings.wifi[slot].ssid.length() == 0) continue;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == deviceSettings.wifi[slot].ssid) {
        candidates[numCandidates].ssid = deviceSettings.wifi[slot].ssid.c_str();
        candidates[numCandidates].pass = deviceSettings.wifi[slot].pass.c_str();
        candidates[numCandidates].rssi = WiFi.RSSI(i);
        numCandidates++;
        break;
      }
    }
  }
  // Insertion sort by RSSI descending — numCandidates is at most
  // kMaxWifiNetworks (4), plain O(n^2) is more than fine here.
  for (int i = 1; i < numCandidates; i++) {
    Candidate key = candidates[i];
    int j = i - 1;
    while (j >= 0 && candidates[j].rssi < key.rssi) {
      candidates[j + 1] = candidates[j];
      j--;
    }
    candidates[j + 1] = key;
  }
  WiFi.scanDelete();

  bool connected = false;
  for (int i = 0; i < numCandidates && !connected; i++) {
    Serial.printf("Connecting to '%s' (%ddBm)", candidates[i].ssid, (int)candidates[i].rssi);
    WiFi.begin(candidates[i].ssid, candidates[i].pass);
    connected = waitForStaConnect();
    if (!connected) WiFi.disconnect();
  }

  // Last-resort fallback: the original hardcoded single SSID, only tried
  // if NO saved network was even visible to attempt — preserves pre-item-H
  // behavior exactly for a fresh/empty NVS (nothing saved yet).
  if (!connected && numCandidates == 0 && strlen(STA_SSID) > 0) {
    Serial.printf("Connecting to %s (fallback default, no saved network visible)", STA_SSID);
    WiFi.begin(STA_SSID, STA_PASS);
    connected = waitForStaConnect();
    if (!connected) diagnoseWifiFailure();
  }

  if (connected) {
    Serial.printf("Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("STA connect failed or no network visible — starting AP fallback.");
    if (toggleTft) renderer.printLine(6, "Starting AP mode...", COLOR_GREY);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("AP '%s' up. IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  // ARDUINO_USB_CDC_ON_BOOT re-enumerates the USB serial port when the
  // sketch starts, so anything printed too early (e.g. from setupWifi()
  // below) can be lost if the monitor hasn't reattached yet. Give it a
  // moment — this is a short bounded wait, not an indefinite block, so
  // boot still proceeds normally with no monitor attached at all.
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) delay(10);

  pinMode(PIN_TFT_BLK, OUTPUT);
  digitalWrite(PIN_TFT_BLK, HIGH); // backlight on

  // TFT init moved ahead of everything that can block (Wi-Fi connect can
  // take up to STA_CONNECT_TIMEOUT_MS) so the panel always shows real
  // content — this used to run after setupWifi(), leaving the backlight on
  // over an uninitialized (blank/garbled) GRAM for the whole connect wait.
  // TFT and SD card share this bus — CS is passed per-device, not to begin().
  displayBus.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
  if (toggleTft) {
    renderer.begin();
    renderer.centeredTitle("OPENSTAT", 40, COLOR_ORANGE, 2);
    renderer.printLine(6, strlen(STA_SSID) > 0 ? "Connecting to Wi-Fi..." : "Starting AP mode...", COLOR_GREY);
  }

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed — did you upload the /data image?");
  }

  loadSweepParams(savedParams); // DPV parameters last saved from the Parameters screen (NVS)
  Serial.printf("DPV params: %.3f..%.3f V, %.0f mV step, %.0f mV pulse, %.0f ms pulse, %.0f ms interval, '%s'\n",
                savedParams.startV, savedParams.endV, savedParams.stepMv, savedParams.pulseAmpMv,
                savedParams.pulseWidthMs, savedParams.intervalMs, savedParams.label.c_str());

  loadDeviceSettings(deviceSettings); // saved Wi-Fi networks (item H) — must load before setupWifi()
  setupWifi();

  afe.begin(PIN_AFE_SCLK, PIN_AFE_MISO, PIN_AFE_MOSI); // sets up the SPI bus on custom pins
  bool afeOk = true; // no real AFE compiled in -> nothing to fail, report OK
#if USE_REAL_AFE
  afeOk = afe.reset();
  if (!afeOk) {
    Serial.println("AD5941 not responding — check wiring / USE_REAL_AFE.");
  }
#endif

  sdReady = SD.begin(PIN_TFT_SDCS, displayBus);
  if (!sdReady) {
    Serial.println("SD card not detected/failed to mount (check the Fermion board's microSD slot).");
  } else {
    Serial.printf("SD card mounted. Size: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
    loadResultsIndex();
  }

  // ---- Onboard GUI: input + state machine + screens ----
  input.begin(PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW, PIN_START, PIN_STOP);

  appCtx.renderer = &renderer;
  appCtx.afe = &afe;
  appCtx.stateMachine = &stateMachine;
  appCtx.afeOk = afeOk;
  appCtx.sdOk = sdReady;
  appCtx.wifiUp = ctxWifiUp;
  appCtx.ipAddress = ctxIpAddress;
  appCtx.wifiIsApMode = ctxWifiIsApMode;
  appCtx.wifiHostForQr = ctxWifiHostForQr;
  appCtx.apJoinString = ctxApJoinString;
  appCtx.rtiaText = ctxRtiaText;
  appCtx.sdReady = ctxSdReady;
  appCtx.batteryPct = ctxBatteryPct;
  appCtx.savedParams = &savedParams;
  appCtx.startSweep = startSweep;
  appCtx.stopSweep = stopSweep;
  appCtx.isSweepRunning = ctxIsSweepRunning;
  appCtx.sweepProgressPct = ctxSweepProgressPct;
  appCtx.sweepElapsedSec = ctxSweepElapsedSec;
  appCtx.currentFilename = ctxCurrentFilename;
  appCtx.currentTechnique = ctxCurrentTechnique;
  appCtx.lastSweepStatus = ctxLastSweepStatus;
  appCtx.lastSweepCompleted = ctxLastSweepCompleted;
  appCtx.savedResultsCount = ctxSavedResultsCount;
  appCtx.savedResultAt = ctxSavedResultAt;
  appCtx.forEachResultPoint = ctxForEachResultPoint;

  stateMachine.registerScreen(ScreenId::BOOT, &screenBoot);
  stateMachine.registerScreen(ScreenId::HOME, &screenHome);
  stateMachine.registerScreen(ScreenId::PARAM_ENTRY, &screenParams);
  stateMachine.registerScreen(ScreenId::POST_SCAN_SUMMARY, &screenSummary);
  stateMachine.registerScreen(ScreenId::FILE_BROWSER, &screenFiles);
  stateMachine.registerScreen(ScreenId::DEVICE_SETTINGS, &screenSettings);

  if (toggleTft) {
    stateMachine.changeTo(ScreenId::BOOT);
  }

  // Serve the dashboard and any static assets from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) { request->send(400, "text/plain", "missing file param"); return; }
    String file = String(RESULTS_DIR) + "/" + request->getParam("file")->value();
    if (sdReady && SD.exists(file)) {
      request->send(SD, file, "text/csv", true);
    } else {
      request->send(404, "text/plain", "not found");
    }
  });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.begin();
  Serial.println("HTTP + WebSocket server started.");
}

static uint32_t serialRxBytes = 0; // total bytes received; shown in the periodic status line

// Runs one completed line from the serial monitor. Every non-empty line
// gets a response so a mistyped command is never silently ignored.
//   selftest              AD5941 DAC->ADC loopback (needs USE_REAL_AFE 1)
//   tfttest               draw a border test pattern on the TFT
//   tftoffset <col> <row> try a different TFT GRAM offset live, then draw the
//                         test pattern (copy good values into TFT_OFFSET_X/Y
//                         in drivers/renderer.h). Reboot afterwards to restore
//                         the normal screen.
//   calibrate <ohms> <rtiaCode>
//                         measure the gain error against a known resistor of
//                         value <ohms> at RTIA range <rtiaCode> (0=200 ..
//                         7=160k) and print the resulting correction factor
//                         (needs USE_REAL_AFE 1) — see AD5941::calibrateGain().
//   debug on|off          toggle per-window ADC averaging diagnostics
//                         (sample count/mean raw code/stddev for both
//                         Ibefore and Iend, every sweep step) — off by
//                         default since it roughly doubles the normal
//                         per-step Serial output.
//   hold <E_volts>        hold a static E (no pulse, no sweep timing) so you
//                         can check E's polarity with a DMM (needs
//                         USE_REAL_AFE 1) — see AD5941::holdStaticE().
//   calpoint <rtiaCode> <E_volts>
//                         fix RTIA to rtiaCode, hold E, settle 3s, average
//                         ~2s of sinc2 samples, print the RAW (uncorrected)
//                         mean current and stddev in nA (needs
//                         USE_REAL_AFE 1) — see AD5941::calPoint(). Run at
//                         >=2 E values against a known resistor to derive
//                         kGainTable/kOffsetTable_nA entries by hand.
//   autorange              run the pre-sweep RTIA auto-ranging probe
//                         standalone, using the currently loaded DPV
//                         params' start/mid/end voltages — no dashboard,
//                         no sweep started (needs USE_REAL_AFE 1). Leaves
//                         RTIA set to whatever it picks.
//   setrtia <rtiaCode>    force fixedRtiaCode to rtiaCode (0=200ohm ..
//                         7=160k) and set autoRange=false, saved to flash —
//                         the non-dashboard way to pick a fixed RTIA range
//                         (e.g. for TFT-only use). Applies to the next
//                         sweep started from either frontend; does not
//                         itself touch the AD5941 (no USE_REAL_AFE needed).
static void handleSerialCommand(String line) {
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) return;
  int col, row;
  float calOhms;
  int calCode;
  float holdE;
  int calPointCode;
  float calPointE;
  int setRtiaCode;
  if (line == "selftest") {
#if USE_REAL_AFE
    if (sweep.running) {
      Serial.println("Cannot self-test while a sweep is running — stop it first.");
    } else {
      afe.selfTestDacAdcLoopback();
    }
#else
    Serial.println("selftest needs USE_REAL_AFE set to 1 in main.cpp.");
#endif
  } else if (line == "tfttest") {
    renderer.testPattern();
    Serial.println("TFT test pattern drawn (reboot to restore the normal screen).");
  } else if (sscanf(line.c_str(), "tftoffset %d %d", &col, &row) == 2) {
    renderer.setOffsets(col, row);
    renderer.testPattern();
    Serial.printf("TFT offset set to col=%d row=%d, test pattern drawn (reboot to restore the normal screen).\n", col, row);
  } else if (sscanf(line.c_str(), "calibrate %f %d", &calOhms, &calCode) == 2) {
#if USE_REAL_AFE
    if (sweep.running) {
      Serial.println("Cannot calibrate while a sweep is running — stop it first.");
    } else if (calCode < 0 || calCode > 7) {
      Serial.println("rtiaCode must be 0..7 (0=200ohm .. 7=160kohm).");
    } else {
      afe.calibrateGain(calOhms, (uint8_t)calCode);
    }
#else
    Serial.println("calibrate needs USE_REAL_AFE set to 1 in main.cpp.");
#endif
  } else if (line == "debug on" || line == "debug off") {
#if USE_REAL_AFE
    bool on = (line == "debug on");
    afe.setDebugWindows(on);
    Serial.printf("Per-window ADC averaging debug output: %s\n", on ? "ON" : "OFF");
#else
    Serial.println("debug needs USE_REAL_AFE set to 1 in main.cpp.");
#endif
  } else if (sscanf(line.c_str(), "hold %f", &holdE) == 1) {
#if USE_REAL_AFE
    if (sweep.running) {
      Serial.println("Cannot hold a static E while a sweep is running — stop it first.");
    } else {
      afe.holdStaticE(holdE);
      Serial.printf("Holding E=%.4fV (VBIAS0=VZERO0-E). Measure RE0 vs SE0 with a DMM to check "
                    "polarity — E more positive should make RE0 more NEGATIVE relative to SE0. "
                    "\"hold 0\" or any onboard/dashboard Start releases it.\n", holdE);
    }
#else
    Serial.println("hold needs USE_REAL_AFE set to 1 in main.cpp.");
#endif
  } else if (sscanf(line.c_str(), "calpoint %d %f", &calPointCode, &calPointE) == 2) {
#if USE_REAL_AFE
    if (sweep.running) {
      Serial.println("Cannot run calpoint while a sweep is running — stop it first.");
    } else if (calPointCode < 0 || calPointCode > 7) {
      Serial.println("rtiaCode must be 0..7 (0=200ohm .. 7=160kohm).");
    } else {
      afe.calPoint((uint8_t)calPointCode, calPointE); // blocks ~5s (3s settle + ~2s capture)
    }
#else
    Serial.println("calpoint needs USE_REAL_AFE set to 1 in main.cpp.");
#endif
  } else if (line == "autorange") {
#if USE_REAL_AFE
    if (sweep.running) {
      Serial.println("Cannot auto-range while a sweep is running — stop it first.");
    } else {
      // Same probe-voltage convention as startSweep()'s own auto-range
      // branch (start/mid/end of the currently loaded DPV params) — this
      // is a standalone way to see/trigger the exact same range selection
      // a TFT-started sweep would use, without starting a sweep or
      // touching the dashboard.
      const float probeVolts[3] = { savedParams.startV, (savedParams.startV + savedParams.endV) * 0.5f,
                                     savedParams.endV };
      AfeRangeResult r = afe.autoRangeRtia(probeVolts, 3);
      Serial.printf("Auto-range: %.0f ohm (code %d)%s, ratiometric correction=%.4f (diagnostic only — "
                    "not applied to readings, see kGainTable/kOffsetTable_nA), peak probe=%.1f nA\n",
                    r.rtiaOhms, r.rtiaCode, r.atMaxRange ? " [AT MAX RANGE — still underscale]" : "",
                    r.correction, r.peakProbeNa);
      Serial.println("RTIA is now set to this range and stays there until reset, a sweep, calibrate, "
                      "or calpoint changes it.");
    }
#else
    Serial.println("autorange needs USE_REAL_AFE set to 1 in main.cpp.");
#endif
  } else if (sscanf(line.c_str(), "setrtia %d", &setRtiaCode) == 1) {
    if (setRtiaCode < 0 || setRtiaCode > 7) {
      Serial.println("rtiaCode must be 0..7 (0=200ohm .. 7=160kohm).");
    } else {
      static const float kOhmsForPrint[8] = {200.0f, 1000.0f, 5000.0f, 10000.0f,
                                              20000.0f, 40000.0f, 80000.0f, 160000.0f};
      savedParams.fixedRtiaCode = (uint8_t)setRtiaCode;
      savedParams.autoRange = false;
      sanitizeSweepParams(savedParams);
      saveSweepParams(savedParams); // persists to NVS — survives reboot, used by both frontends
      Serial.printf("RTIA forced to code %d (%.0f ohm), autoRange disabled and saved to flash. "
                    "Applies to the next sweep started from the TFT or dashboard.\n",
                    setRtiaCode, kOhmsForPrint[setRtiaCode]);
    }
  } else {
    Serial.printf("Unknown command \"%s\" — try: selftest, tfttest, tftoffset <col> <row>, "
                  "calibrate <ohms> <rtiaCode>, debug on|off, hold <E_volts>, "
                  "calpoint <rtiaCode> <E_volts>, autorange, setrtia <rtiaCode>\n", line.c_str());
  }
}

// ---------------------------------------------------------------------
void loop() {
  ws.cleanupClients();

  // ---- Onboard GUI: drain input events into whichever screen is active ----
  input.update();
  InputEvent ev;
  while (input.poll(ev)) {
    // Item A — STOP_BTN stops a running sweep regardless of which screen
    // is active, before the per-screen dispatch below (which only reaches
    // whichever Screen is current, and not every screen forwards STOP_BTN
    // to stopSweep() — e.g. File Browser/Settings repurpose it as "back").
    // Mirrors the web dashboard's "stop" WS command, which already calls
    // stopSweep() directly and unconditionally.
    if (ev.type == InputType::STOP_BTN && sweep.running) {
      stopSweep(false);
      continue; // still consumed as "stop", not also forwarded as "back"/etc.
    }
    stateMachine.onInput(ev);
  }
  stateMachine.update();

  // Serial bench commands (see handleSerialCommand()). Characters are
  // accumulated into a line buffer without ever blocking (some monitors send
  // each keystroke as typed, so a blocking readStringUntil() would split
  // slowly-typed commands), and every completed line gets a response so a
  // mistyped or mis-delivered command is never silently ignored.
  static String serialLine;
  static unsigned long lastRxMs = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    serialRxBytes++;
    lastRxMs = millis();
    if (c == '\n' || c == '\r') {
      handleSerialCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 64) {
      serialLine += c;
    }
  }
  // Terminals set to "append nothing" (Termite's default option) never send
  // a newline, so treat 300ms of silence after the last character as
  // end-of-line instead of waiting forever.
  if (serialLine.length() > 0 && millis() - lastRxMs > 300) {
    handleSerialCommand(serialLine);
    serialLine = "";
  }

  serviceSweep();

  // Suspended entirely while a sweep is running (rather than just skipping
  // the TFT chrome redraw, which was already gated) — this is a WS
  // broadcast plus a handful of SPI-free reads, but it's still WiFi/TCP
  // work that has no reason to compete with the sweep for CPU time between
  // points; the next tick after the sweep ends resumes it normally.
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 2000 && !sweep.running) {
    lastStatus = millis();
    sendStatus();
  }

  // Re-print the address periodically (not just once at boot) so it's
  // visible even if the serial monitor was attached after startup —
  // see the USB-CDC re-enumeration note in setup().
  static unsigned long lastIpPrint = 0;
  if (millis() - lastIpPrint > 5000) {
    lastIpPrint = millis();
    if (WiFi.getMode() == WIFI_AP) {
      Serial.printf("AP '%s' IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    } else if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("STA IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("Wi-Fi not connected and no AP active.");
    }
    Serial.printf("Serial commands: selftest, tfttest, tftoffset <col> <row>, calibrate <ohms> "
                  "<rtiaCode>, debug on|off, hold <E_volts>, calpoint <rtiaCode> <E_volts>, "
                  "autorange  (RX bytes so far: %u)\n",
                  (unsigned)serialRxBytes);
  }
}
