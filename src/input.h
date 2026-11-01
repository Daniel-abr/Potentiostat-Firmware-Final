/*
 * input.h — shared input-handling module used by every screen.
 *
 * Wraps the rotary encoder (quadrature A/B + integrated push-button) and
 * the two dedicated Start/Stop buttons into one normalized InputEvent
 * queue (see state_machine.h). Screens never read GPIOs directly.
 *
 * Quadrature decoding runs in a pair of ISRs (cheap, glitch-tolerant
 * enough for a UI encoder — for a production-grade encoder driver, swap
 * this for a proper library such as ESP32Encoder or a full Gray-code
 * state table). Button debouncing and short/long/double-click
 * classification run in update(), which must be called every loop().
 *
 * Usage:
 *   InputManager input;
 *   input.begin(PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW, PIN_SW1, PIN_SW2);
 *   ...
 *   // in loop():
 *   input.update();
 *   InputEvent ev;
 *   while (input.poll(ev)) stateMachine.onInput(ev);
 */
#pragma once
#include <Arduino.h>
#include "state_machine.h" // InputEvent / InputType

class InputManager {
public:
  void begin(uint8_t pinA, uint8_t pinB, uint8_t pinSw, uint8_t pinStart, uint8_t pinStop);

  // Call every loop() iteration. Services debounce timers and drains
  // accumulated encoder ticks into ROTATE_* events.
  void update();

  // Pop one event from the internal queue. Call in a while-loop — a
  // single update() can generate several events (e.g. multiple encoder
  // detents rotated quickly).
  bool poll(InputEvent &out);

  // Item A — STOP_BTN is latched by a real GPIO interrupt (isrStopBtn,
  // IRAM_ATTR) independent of update()'s polling cadence, specifically so
  // a press isn't lost if it lands entirely inside one of this firmware's
  // rare but real multi-second blocking stretches (AD5941::autoRangeRtia()'s
  // probe) where update() itself doesn't run for a while. update() still
  // turns this into the normal STOP_BTN InputEvent once it next runs, same
  // as any other button — this pair of methods is for code that needs to
  // notice a stop request *during* a blocking stretch, before update() gets
  // a chance to (see AD5941::autoRangeRtia()'s stopRequested parameter).
  //
  // isStopRequested() only peeks — repeated calls (e.g. once per probe
  // step) keep seeing it until explicitly cleared, so a long-running
  // caller can poll it directly without racing update()'s own, separate
  // consume-into-an-event behavior. clearStopRequest() is for discarding a
  // stale press from before a new sweep starts (e.g. STOP used moments
  // earlier as "back" in a menu) so it can't be mistaken for a request to
  // abort the sweep that's only just beginning.
  bool isStopRequested() const { return s_stopFlag; }
  void clearStopRequest() { s_stopFlag = false; }

private:
  static void IRAM_ATTR isrEncoderA();
  static void IRAM_ATTR isrEncoderB();
  static void IRAM_ATTR isrStopBtn();

  void pushEvent(InputType t);
  void serviceEncoderButton();
  void serviceEdgeButton(uint8_t pin, bool &lastRaw, bool &stable,
                          unsigned long &lastChangeMs, InputType pressEvent);

  static const uint8_t QUEUE_LEN = 16;
  InputEvent _queue[QUEUE_LEN];
  uint8_t _qHead = 0, _qTail = 0;

  uint8_t _pinSw = 0, _pinStart = 0, _pinStop = 0;

  // Pins read from the ISR context — static so the (necessarily static)
  // ISR methods can reach them without a `this` pointer.
  static uint8_t s_pinA, s_pinB;
  static volatile int32_t s_encDelta; // raw quadrature edge count since last drain

  // STOP_BTN ISR latch (item A) — see isStopRequested()/clearStopRequest()
  // above. s_pinStop mirrors _pinStop for the same static-ISR-context
  // reason as s_pinA/s_pinB. s_lastStopIsrMs is a simple timestamp lockout
  // inside the ISR itself (no delay() allowed there) to avoid contact
  // bounce re-triggering it uselessly — not load-bearing for correctness,
  // since the flag is a plain idempotent latch, just avoids redundant
  // ISR work.
  static uint8_t s_pinStop;
  static volatile bool s_stopFlag;
  static volatile unsigned long s_lastStopIsrMs;
  static const unsigned long STOP_ISR_LOCKOUT_MS = 50;

  // Encoder push-button: short / long / double-click state
  bool _swLastRaw = true, _swStable = true; // idle == HIGH (INPUT_PULLUP, active-low)
  unsigned long _swLastChangeMs = 0;
  unsigned long _swPressStartMs = 0;
  bool _swLongFired = false;
  bool _swAwaitingSecondClick = false;
  unsigned long _swFirstClickMs = 0;

  // Start / Stop buttons: simple debounced press-edge detection
  bool _startLastRaw = true, _startStable = true;
  unsigned long _startLastChangeMs = 0;
  bool _stopLastRaw = true, _stopStable = true;
  unsigned long _stopLastChangeMs = 0;

  static const unsigned long DEBOUNCE_MS = 30;
  static const unsigned long LONG_PRESS_MS = 800;
  static const unsigned long DOUBLE_CLICK_MS = 400;
  static const int32_t COUNTS_PER_DETENT = 4; // most cheap encoders: 4 edges/detent
};
