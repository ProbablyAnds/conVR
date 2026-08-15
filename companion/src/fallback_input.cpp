#include "fallback_input.h"

#include <algorithm>
#include <cmath>

#include "config.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// windows.h defines max() as a macro, which turns any later std::max into a
// syntax error.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace convr {
namespace {

const KeyChoice kMovementKeys[] = {
    {"W", 0x11},
    {"S", 0x1F},
    {"A", 0x1E},
    {"D", 0x20},
    {"Space", 0x39},
};

const KeyChoice kModifierKeys[] = {
    {"Left Shift", 0x2A},
    {"Left Alt", 0x38},
    {"Left Ctrl", 0x1D},
    {"E", 0x12},
    {"(none)", 0},
};

std::string ToLowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return text;
}

// "SkyrimVR", "skyrimvr.exe" and "C:\...\SkyrimVR.exe" all have to compare
// equal, because the user types whichever of those they happen to know.
std::string NormalizeProcessName(std::string name) {
  name = ToLowerAscii(std::move(name));
  const size_t slash = name.find_last_of("\\/");
  if (slash != std::string::npos) name = name.substr(slash + 1);
  const size_t dot = name.rfind(".exe");
  if (dot != std::string::npos && dot + 4 == name.size()) {
    name = name.substr(0, dot);
  }
  return name;
}

}  // namespace

const KeyChoice* MovementKeyChoices(int* count) {
  if (count) *count = static_cast<int>(sizeof(kMovementKeys) / sizeof(KeyChoice));
  return kMovementKeys;
}

const KeyChoice* ModifierKeyChoices(int* count) {
  if (count) *count = static_cast<int>(sizeof(kModifierKeys) / sizeof(KeyChoice));
  return kModifierKeys;
}

const char* KeyLabelForScancode(int scancode) {
  for (const KeyChoice& key : kMovementKeys)
    if (key.scancode == scancode) return key.label;
  for (const KeyChoice& key : kModifierKeys)
    if (key.scancode == scancode) return key.label;
  return "(custom)";
}

bool ForegroundProcessMatches(const std::string& process_name) {
#if defined(_WIN32)
  if (process_name.empty()) return false;
  HWND foreground = GetForegroundWindow();
  if (foreground == nullptr) return false;

  DWORD pid = 0;
  GetWindowThreadProcessId(foreground, &pid);
  if (pid == 0) return false;

  HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) return false;

  char path[MAX_PATH] = {};
  DWORD size = MAX_PATH;
  const bool ok = QueryFullProcessImageNameA(process, 0, path, &size) != 0;
  CloseHandle(process);
  if (!ok) return false;

  return NormalizeProcessName(path) == NormalizeProcessName(process_name);
#else
  (void)process_name;
  return false;
#endif
}

KeyboardFallback::~KeyboardFallback() { ReleaseAll(); }

void KeyboardFallback::HoldKey(int scancode, bool want_down, bool* held) {
  if (scancode == 0) {
    *held = false;
    return;
  }
  if (want_down == *held) return;

#if defined(_WIN32)
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wScan = static_cast<WORD>(scancode);
  // Games that read raw/DirectInput keyboard state look at the scancode, not
  // the virtual key, so the virtual key is deliberately left zero.
  input.ki.dwFlags = KEYEVENTF_SCANCODE | (want_down ? 0 : KEYEVENTF_KEYUP);
  if (SendInput(1, &input, sizeof(INPUT)) != 1) return;
#endif

  *held = want_down;
}

void KeyboardFallback::ReleaseAll() {
  if (forward_held_) HoldKey(held_forward_scancode_, false, &forward_held_);
  if (back_held_) HoldKey(held_back_scancode_, false, &back_held_);
  if (sprint_held_) HoldKey(held_sprint_scancode_, false, &sprint_held_);
  forward_release_at_ = Clock::time_point{};
  back_release_at_ = Clock::time_point{};
  sprint_release_at_ = Clock::time_point{};
}

bool KeyboardFallback::Latch(bool above_press, bool above_release, bool held,
                             Clock::time_point* release_at, int hold_ms) {
  if (above_press) {
    // Any fresh step cancels a pending release outright.
    *release_at = Clock::time_point{};
    return true;
  }
  if (!held) return false;
  // Between the release and press thresholds the key simply stays down; the
  // countdown only starts once the belt has genuinely gone quiet.
  if (above_release) {
    *release_at = Clock::time_point{};
    return true;
  }
  const Clock::time_point now = Clock::now();
  if (*release_at == Clock::time_point{}) {
    *release_at = now + std::chrono::milliseconds(hold_ms);
  }
  // Deliberately not an early "return true" on the frame the deadline is set:
  // that would grant an extra frame of hold, and would make hold_ms=0 never
  // release at all until a second update came in.
  return now < *release_at;
}

void KeyboardFallback::Update(const Config& config, float joystick_y,
                              bool sprint) {
#if !defined(_WIN32)
  (void)joystick_y;
  (void)sprint;
  if (config.fallback_enabled) {
    status_ = "keyboard fallback is Windows-only";
  } else {
    status_ = "disabled";
  }
  ReleaseAll();
  return;
#else
  if (!config.fallback_enabled) {
    ReleaseAll();
    target_focused_ = false;
    status_ = "disabled";
    return;
  }

  // Remember which scancode each key is being held with, so changing the key
  // in the UI mid-hold releases the one actually down rather than the new one.
  target_focused_ = ForegroundProcessMatches(config.fallback_target_process);
  if (config.fallback_require_focus && !target_focused_) {
    ReleaseAll();
    status_ = "waiting for " + config.fallback_target_process + " to be focused";
    return;
  }

  const float press = std::max(config.fallback_threshold, 0.01f);
  // Releasing at the same level it presses at would make the key chatter while
  // the belt hovers on the threshold, so release is deliberately lower.
  const float release = press * 0.5f;
  const int hold_ms = std::max(config.fallback_hold_ms, 0);

  const bool want_forward =
      Latch(joystick_y > press, joystick_y > release, forward_held_,
            &forward_release_at_, hold_ms);
  const bool want_back =
      Latch(joystick_y < -press, joystick_y < -release, back_held_,
            &back_release_at_, hold_ms);
  // Sprint follows the same latch so it does not flicker off between steps
  // while the belt is still clearly at running speed.
  const bool want_sprint =
      want_forward && Latch(sprint, sprint, sprint_held_, &sprint_release_at_,
                            hold_ms);

  if (!forward_held_) held_forward_scancode_ = config.fallback_forward_key;
  if (!back_held_) held_back_scancode_ = config.fallback_back_key;
  if (!sprint_held_) held_sprint_scancode_ = config.fallback_sprint_key;

  HoldKey(held_forward_scancode_, want_forward, &forward_held_);
  HoldKey(held_back_scancode_, want_back, &back_held_);
  HoldKey(held_sprint_scancode_, want_sprint, &sprint_held_);

  if (forward_held_ || back_held_) {
    status_ = std::string("sending ") +
              (forward_held_ ? KeyLabelForScancode(held_forward_scancode_)
                             : KeyLabelForScancode(held_back_scancode_)) +
              (sprint_held_ ? " + sprint" : "");
  } else {
    status_ = config.fallback_require_focus ? "armed, belt below threshold"
                                            : "armed (focus check off)";
  }
#endif
}

}  // namespace convr
