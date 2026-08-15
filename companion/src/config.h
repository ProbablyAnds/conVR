// Tuning settings, persisted as a flat INI file. The format is deliberately
// dumb text so the same file copies straight from Linux to Windows.
#pragma once

#include <string>

namespace convr {

enum class SprintSource : int {
  kHardwareButton = 0,  // the firmware's own speed-threshold button
  kSoftwareThreshold = 1,
};

struct Config {
  // Matched case-insensitively as a substring of the SDL joystick name, so
  // "conVR Treadmill Controller" finds "Raspberry Pi conVR Treadmill
  // Controller" without the user having to type the vendor prefix.
  std::string device_name = "conVR Treadmill Controller";

  int speed_axis = 1;     // SDL axis index carrying belt speed
  int sprint_button = 0;  // SDL button index for the firmware sprint button

  // The firmware drives the axis negative when walking forward; SteamVR wants
  // positive-Y forward, so this defaults on.
  bool invert_forward = true;

  float deadzone = 0.05f;       // normalized, kills idle jitter around centre
  float max_input = 1.0f;       // normalized axis value that means "full stick"
  float curve_exponent = 1.0f;  // 1.0 = linear; >1 softens the low end

  SprintSource sprint_source = SprintSource::kHardwareButton;
  float sprint_threshold = 0.55f;  // output speed at which software sprint fires

  // --- keyboard fallback (see fallback_input.h) ---
  // Off by default: this is the escape hatch for games that cannot read the
  // treadmill through SteamVR, not the normal path.
  bool fallback_enabled = false;
  float fallback_threshold = 0.15f;  // output speed at which the key goes down
  // A walking belt is driven in pulses - one per footfall - so the speed dips
  // between steps. Without a hold the key would be released in every dip and
  // the character would stutter instead of walking.
  int fallback_hold_ms = 400;
  int fallback_forward_key = 0x11;   // W
  int fallback_back_key = 0x1F;      // S
  int fallback_sprint_key = 0x2A;    // Left Shift
  // Synthetic keystrokes go to whatever window has focus, so by default they
  // are only sent while the game itself is focused. Turning this off means the
  // treadmill types into whatever you have open.
  bool fallback_require_focus = true;
  std::string fallback_target_process = "SkyrimVR";

  // Reads `path`. Missing keys keep their default; a missing file is not an
  // error, it just means "first run".
  bool Load(const std::string& path, std::string* error);
  bool Save(const std::string& path, std::string* error) const;
};

// %APPDATA%\convr\companion.ini or ~/.config/convr/companion.ini.
std::string DefaultConfigPath();

}  // namespace convr
