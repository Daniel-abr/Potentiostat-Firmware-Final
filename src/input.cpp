#include "input.h"

// Flip to 1 to reverse which physical rotation direction (CW/CCW) maps to
// ROTATE_CW/ROTATE_CCW — a single switch, easy to flip back (confirmed
// with the user). Applied as a sign flip on the raw quadrature delta in
// both ISRs below, upstream of everything else (detent accumulation,
// event queueing), so nothing downstream needs to know this exists.
#define ENCODER_REVERSE 1
#if ENCODER_REVERSE
static const int32_t kEncoderDir = -1;
#else
static const int32_t kEncoderDir = 1;
#endif

uint8_t InputManager::s_pinA = 0;
uint8_t InputManager::s_pinB = 0;
volatile int32_t InputManager::s_encDelta = 0;

uint8_t InputManager::s_pinStop = 0;
volatile bool InputManager::s_stopFlag = false;
volatile unsigned long InputManager::s_lastStopIsrMs = 0;

void IRAM_ATTR InputManager::isrEncoderA() {
  bool a = digitalRead(s_pinA);
  bool b = digitalRead(s_pinB);
  s_encDelta += kEncoderDir * ((a == b) ? 1 : -1);
}

void IRAM_ATTR InputManager::isrEncoderB() {
  bool a = digitalRead(s_pinA);
  bool b = digitalRead(s_pinB);
  s_encDelta += kEncoderDir * ((a != b) ? 1 : -1);
}

// Item A — fires on every edge (attached CHANGE, same as the encoder pins,
// since attachInterrupt's FALLING mode is not reliably available on every
// ESP32-S3 GPIO/driver combo); only the falling (pressed) edge actually
// latches the flag. No delay() here (not allowed in an ISR) — the lockout
// below is a plain millis() timestamp comparison.
void IRAM_ATTR InputManager::isrStopBtn() {
  if (digitalRead(s_pinStop) != LOW) return; // active-low (INPUT_PULLUP) — only act on press
  unsigned long now = millis();
  if (now - s_lastStopIsrMs < STOP_ISR_LOCKOUT_MS) return;
  s_lastStopIsrMs = now;
  s_stopFlag = true;
}

void InputManager::begin(uint8_t pinA, uint8_t pinB, uint8_t pinSw, uint8_t pinStart, uint8_t pinStop) {
  s_pinA = pinA;
  s_pinB = pinB;
  s_pinStop = pinStop;
  _pinSw = pinSw;
  _pinStart = pinStart;
  _pinStop = pinStop;

  pinMode(pinA, INPUT_PULLUP);
  pinMode(pinB, INPUT_PULLUP);
  pinMode(_pinSw, INPUT_PULLUP);
  pinMode(_pinStart, INPUT_PULLUP);
  pinMode(_pinStop, INPUT_PULLUP);

  _swLastRaw = _swStable = digitalRead(_pinSw);
  _startLastRaw = _startStable = digitalRead(_pinStart);
  _stopLastRaw = _stopStable = digitalRead(_pinStop);

  attachInterrupt(digitalPinToInterrupt(pinA), isrEncoderA, CHANGE);
  attachInterrupt(digitalPinToInterrupt(pinB), isrEncoderB, CHANGE);
  attachInterrupt(digitalPinToInterrupt(pinStop), isrStopBtn, CHANGE);
}

void InputManager::pushEvent(InputType t) {
  uint8_t next = (uint8_t)((_qHead + 1) % QUEUE_LEN);
  if (next == _qTail) return; // queue full — drop rather than block
  _queue[_qHead].type = t;
  _qHead = next;
}

bool InputManager::poll(InputEvent &out) {
  if (_qTail == _qHead) return false;
  out = _queue[_qTail];
  _qTail = (uint8_t)((_qTail + 1) % QUEUE_LEN);
  return true;
}

void InputManager::serviceEdgeButton(uint8_t pin, bool &lastRaw, bool &stable,
                                      unsigned long &lastChangeMs, InputType pressEvent) {
  unsigned long now = millis();
  bool raw = digitalRead(pin);
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeMs = now;
  }
  if (now - lastChangeMs > DEBOUNCE_MS && stable != raw) {
    stable = raw;
    if (!stable) pushEvent(pressEvent); // LOW == pressed (active-low, INPUT_PULLUP)
  }
}

void InputManager::serviceEncoderButton() {
  unsigned long now = millis();
  bool raw = digitalRead(_pinSw); // LOW == pressed

  if (raw != _swLastRaw) {
    _swLastRaw = raw;
    _swLastChangeMs = now;
  }

  if (now - _swLastChangeMs > DEBOUNCE_MS && _swStable != raw) {
    _swStable = raw;
    if (!_swStable) {
      // Falling edge: press started.
      _swPressStartMs = now;
    } else {
      // Rising edge: press released. A long-press already fired (below)
      // while the button was still held, so only classify short taps here.
      unsigned long heldMs = now - _swPressStartMs;
      if (heldMs < LONG_PRESS_MS) {
        if (_swAwaitingSecondClick && (now - _swFirstClickMs) <= DOUBLE_CLICK_MS) {
          pushEvent(InputType::DOUBLE_CLICK);
          _swAwaitingSecondClick = false;
        } else {
          // Hold this tap back — it might be the first half of a
          // double-click. Resolved as SHORT_PRESS below if the window
          // expires with no second click.
          _swAwaitingSecondClick = true;
          _swFirstClickMs = now;
        }
      }
    }
  }

  // Long-press fires the moment the hold threshold is crossed, while still
  // held, so a screen can react immediately (e.g. "back") without waiting
  // for release.
  if (!_swStable) {
    if (!_swLongFired && (now - _swPressStartMs) >= LONG_PRESS_MS) {
      pushEvent(InputType::LONG_PRESS);
      _swLongFired = true;
      _swAwaitingSecondClick = false; // a long hold cancels any pending click
    }
  } else {
    _swLongFired = false;
  }

  // Resolve a pending single click once the double-click window expires.
  if (_swAwaitingSecondClick && (now - _swFirstClickMs) > DOUBLE_CLICK_MS) {
    pushEvent(InputType::SHORT_PRESS);
    _swAwaitingSecondClick = false;
  }
}

void InputManager::update() {
  // ---- Rotation: drain accumulated quadrature edges into detents ----
  noInterrupts();
  int32_t delta = s_encDelta;
  s_encDelta = 0;
  interrupts();

  static int32_t accum = 0;
  accum += delta;
  while (accum >= COUNTS_PER_DETENT) {
    pushEvent(InputType::ROTATE_CW);
    accum -= COUNTS_PER_DETENT;
  }
  while (accum <= -COUNTS_PER_DETENT) {
    pushEvent(InputType::ROTATE_CCW);
    accum += COUNTS_PER_DETENT;
  }

  // ---- Encoder button: short / long / double-click ----
  serviceEncoderButton();

  // ---- Start / Stop: simple debounced press-edge ----
  serviceEdgeButton(_pinStart, _startLastRaw, _startStable, _startLastChangeMs, InputType::START_BTN);
  serviceEdgeButton(_pinStop, _stopLastRaw, _stopStable, _stopLastChangeMs, InputType::STOP_BTN);

  // ---- Item A: turn a latched ISR stop request into the normal event ----
  // Covers a press that happened while update() itself wasn't being called
  // (a blocking stretch elsewhere) — the debounced path above already
  // handles the ordinary case, so this is a supplement, not a replacement.
  // Consumed (cleared) here so it becomes exactly one event, not re-pushed
  // every tick for as long as the flag stays latched.
  if (s_stopFlag) {
    s_stopFlag = false;
    pushEvent(InputType::STOP_BTN);
  }
}
