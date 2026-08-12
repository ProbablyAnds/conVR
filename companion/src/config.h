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

  // Reads `path`. Missing keys keep their default; a missing file is not an
  // error, it just means "first run".
  bool Load(const std::string& path, std::string* error);
  bool Save(const std::string& path, std::string* error) const;
};

// %APPDATA%\convr\companion.ini or ~/.config/convr/companion.ini.
std::string DefaultConfigPath();

}  // namespace convr
