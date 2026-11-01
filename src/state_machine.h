/*
 * state_machine.h — screen management for the OpenStat display firmware.
 *
 * The whole UI is a small set of full-screen states (Boot, Home, Param
 * Entry, ...). Each one is a `Screen` subclass with enter()/exit()/update()/
 * onInput(). The `StateMachine` owns a fixed table of screens (registered
 * once in setup(), never allocated/freed at runtime) and just tracks which
 * one is currently active, forwarding update() ticks and input events to it.
 *
 * Adding a new screen later = add an id to ScreenId, subclass Screen, and
 * register an instance in main.cpp's setup(). Nothing else needs to change.
 */
#pragma once
#include <Arduino.h>

// One entry per screen in the UI. Order/value doesn't matter, only
// uniqueness — used as an index into StateMachine's internal table.
enum class ScreenId {
  BOOT = 0,
  HOME,
  PARAM_ENTRY,
  POST_SCAN_SUMMARY,
  FILE_BROWSER,
  DEVICE_SETTINGS,
  COUNT // sentinel — keep last, used to size the screen table
};

// Normalized input vocabulary produced by InputManager (see input.h) and
// consumed by screens. Screens never touch raw GPIO/encoder state — this
// is the only thing they see, which keeps input handling identical no
// matter which screen is active.
enum class InputType {
  ROTATE_CW,
  ROTATE_CCW,
  SHORT_PRESS,   // encoder button tap, < 500 ms
  LONG_PRESS,    // encoder button held, > 800 ms (fires once, while held)
  DOUBLE_CLICK,  // two short presses within 400 ms
  START_BTN,     // dedicated Start button pressed
  STOP_BTN       // dedicated Stop button pressed
};

struct InputEvent {
  InputType type;
};

// Base class for every screen. Keep implementations non-blocking — no
// delay() — since the AD5941 sweep, WebSocket server, and encoder polling
// all need to keep running while a screen is active.
class Screen {
public:
  virtual ~Screen() {}
  virtual void enter() {}                          // switched to
  virtual void exit() {}                            // switched away from
  virtual void update() {}                          // every loop() tick
  virtual void onInput(const InputEvent &ev) {}      // one event at a time
};

class StateMachine {
public:
  void registerScreen(ScreenId id, Screen *screen) {
    size_t idx = (size_t)id;
    if (idx < (size_t)ScreenId::COUNT) _screens[idx] = screen;
  }

  void changeTo(ScreenId id) {
    size_t idx = (size_t)id;
    if (idx >= (size_t)ScreenId::COUNT || !_screens[idx]) return; // unregistered — ignore
    if (_current) _current->exit();
    _currentId = id;
    _current = _screens[idx];
    _current->enter();
  }

  void update() {
    if (_current) _current->update();
  }

  void onInput(const InputEvent &ev) {
    if (_current) _current->onInput(ev);
  }

  ScreenId currentId() const { return _currentId; }
  Screen *current() const { return _current; }

private:
  Screen *_screens[(size_t)ScreenId::COUNT] = { nullptr };
  Screen *_current = nullptr;
  ScreenId _currentId = ScreenId::COUNT; // "none yet"
};
