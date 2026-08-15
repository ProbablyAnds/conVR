// Last-resort locomotion path for games that cannot see the treadmill through
// SteamVR's input system.
//
// Legacy-input titles (Skyrim VR among them) build their controller state from
// the two hand devices only, so a treadmill-role device can be bound in the
// SteamVR UI and still never reach the game. When that happens the treadmill
// has to pretend to be something the game already reads. Synthetic keystrokes
// are the cheapest such thing: no kernel driver, no extra install, and every
// PC title takes them.
//
// Off by default. It is deliberately a fallback, not the main path - the
// SteamVR route is analog and this one is not.
#pragma once

#include <string>

namespace convr {

struct Config;

// Scancodes the UI offers. Set-1 ("make") codes, non-extended only, which is
// what games reading raw keyboard input expect.
struct KeyChoice {
  const char* label;
  int scancode;
};
const KeyChoice* MovementKeyChoices(int* count);
const KeyChoice* ModifierKeyChoices(int* count);
const char* KeyLabelForScancode(int scancode);

class KeyboardFallback {
 public:
  ~KeyboardFallback();

  KeyboardFallback(const KeyboardFallback&) = delete;
  KeyboardFallback& operator=(const KeyboardFallback&) = delete;
  KeyboardFallback() = default;

  // Call once per frame with the same values being streamed to the driver.
  // Releases everything and does nothing else when disabled.
  void Update(const Config& config, float joystick_y, bool sprint);

  // Lets go of every key this class is holding. Safe to call repeatedly, and
  // called from the destructor so a crash-free exit never leaves a key stuck.
  void ReleaseAll();

  bool forward_held() const { return forward_held_; }
  bool back_held() const { return back_held_; }
  bool sprint_held() const { return sprint_held_; }
  // Whether the configured target window currently has focus. Meaningless when
  // require_focus is off.
  bool target_focused() const { return target_focused_; }
  // Human-readable reason the fallback is or is not sending right now.
  const std::string& status() const { return status_; }

 private:
  void HoldKey(int scancode, bool want_down, bool* held);

  bool forward_held_ = false;
  bool back_held_ = false;
  bool sprint_held_ = false;
  bool target_focused_ = false;
  int held_forward_scancode_ = 0;
  int held_back_scancode_ = 0;
  int held_sprint_scancode_ = 0;
  std::string status_ = "idle";
};

// True when the foreground window belongs to a process whose executable name
// matches `process_name` (case-insensitive, ".exe" optional). Always false on
// platforms without a window manager we can ask.
bool ForegroundProcessMatches(const std::string& process_name);

}  // namespace convr
