/*
 * app_context.h — everything a screen is allowed to touch.
 *
 * Screens don't reach into main.cpp's globals (sweep state, SD card,
 * WebSocket server, ...) directly. Instead main.cpp fills in one
 * AppContext struct at boot and hands every screen a reference to it.
 * That keeps screens testable/movable in isolation and keeps the
 * sweep-engine / SD / networking code in main.cpp from needing to know
 * anything about the UI beyond "call these function pointers".
 *
 * Plain C-style function pointers (rather than std::function) are used
 * for the callbacks — cheaper on an MCU and every one of them just wraps
 * an existing free function in main.cpp, so no captures are needed.
 */
#pragma once
#include <Arduino.h>

class Renderer;
class AD5941;
class StateMachine;
struct SweepParams; // defined in main.cpp

// Called once per data row when streaming a saved sweep from SD.
typedef void (*PointVisitor)(float voltage, float currentNa, void *user);

struct AppContext {
  // ---- Shared drivers / state machine ----
  Renderer *renderer = nullptr;
  AD5941 *afe = nullptr;
  StateMachine *stateMachine = nullptr;

  // ---- One-shot results from setup()'s self-tests (read by Boot) ----
  bool afeOk = false;
  bool sdOk = false;

  // ---- Live status (read by any screen's chrome/status line) ----
  bool (*wifiUp)() = nullptr;
  // Short text for the web dashboard's address, e.g. "IP 192.168.1.20" or "AP 192.168.4.1".
  const char *(*ipAddress)() = nullptr;
  bool (*sdReady)() = nullptr;
  int (*batteryPct)() = nullptr;
  // Short TFT-safe text for the active HSTIA gain resistor (auto-ranged
  // per sweep, see AD5941::autoRangeRtia()), e.g. "10kR" or "200R" — ASCII
  // only, since the TFT's built-in font can't render "Ω" (unlike the web
  // dashboard's tiaGain field, which uses the real Ω glyph in-browser).
  const char *(*rtiaText)() = nullptr;
  // Item I (QR code) — wifiUp above only means "some Wi-Fi mode is up at
  // all" (AP counts too); this specifically distinguishes AP fallback from
  // a real STA connection. wifiHostForQr() returns the bare IP with no
  // "AP "/"IP " prefix (unlike ipAddress() above, which is prefixed for
  // on-screen display) — ready to drop straight into a "http://<ip>/" QR
  // payload. apJoinString() is only meaningful in AP mode: a ready-made
  // "WIFI:S:<ssid>;T:WPA;P:<pass>;;" string for a Wi-Fi-join QR.
  bool (*wifiIsApMode)() = nullptr;
  const char *(*wifiHostForQr)() = nullptr;
  const char *(*apJoinString)() = nullptr;

  // ---- DPV parameters used for the next sweep, and the live sweep engine
  //      already implemented in main.cpp (startSweep/stopSweep/etc.) ----
  SweepParams *savedParams = nullptr;
  // Returns true only if the sweep actually started running (false for any
  // bail-out — already running, STOP pressed mid-probe, configureSweep()
  // refusing the timing) — see main.cpp's startSweep() doc comment. Screens
  // must check this before assuming a sweep is now in progress.
  bool (*startSweep)(const SweepParams &p) = nullptr;
  void (*stopSweep)(bool completed) = nullptr;
  bool (*isSweepRunning)() = nullptr;
  int (*sweepProgressPct)() = nullptr;
  unsigned long (*sweepElapsedSec)() = nullptr;
  const char *(*currentFilename)() = nullptr;
  const char *(*currentTechnique)() = nullptr;
  // Item B — the most recent completed/stopped/aborted sweep's outcome,
  // read by ScreenPostScanSummary so it can show which of "SWEEP
  // COMPLETE" / "STOPPED" / "OVERLOAD" / "ERROR" actually happened instead
  // of a single hardcoded string. lastSweepStatus() returns "ok" /
  // "overload" / "error" (same values as SavedResult::status);
  // lastSweepCompleted() distinguishes a true completion from a plain
  // user-initiated stop when status is "ok".
  const char *(*lastSweepStatus)() = nullptr;
  bool (*lastSweepCompleted)() = nullptr;

  // ---- Saved results (read by the File Browser) ----
  int (*savedResultsCount)() = nullptr;
  // Copies the idx-th saved result (0 = newest) into the caller's buffers.
  // Returns false if idx is out of range.
  bool (*savedResultAt)(int idx, char *file, size_t fileLen, char *peak, size_t peakLen) = nullptr;
  // Streams every (voltage, current_nA) row of a saved sweep's CSV from the
  // SD card to `visit`, in file order, without holding the sweep in RAM.
  // Returns false if the file couldn't be opened.
  bool (*forEachResultPoint)(const char *file, PointVisitor visit, void *user) = nullptr;
};
