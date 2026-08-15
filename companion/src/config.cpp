#include "config.h"

#include <stdlib.h>

#include <filesystem>
#include <fstream>

namespace convr {
namespace {

std::string Trim(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

}  // namespace

std::string DefaultConfigPath() {
  namespace fs = std::filesystem;
#if defined(_WIN32)
  const char* appdata = getenv("APPDATA");
  fs::path base = appdata ? fs::path(appdata) : fs::path(".");
#else
  const char* xdg = getenv("XDG_CONFIG_HOME");
  const char* home = getenv("HOME");
  fs::path base;
  if (xdg && *xdg) {
    base = fs::path(xdg);
  } else if (home && *home) {
    base = fs::path(home) / ".config";
  } else {
    base = fs::path(".");
  }
#endif
  return (base / "convr" / "companion.ini").string();
}

bool Config::Load(const std::string& path, std::string* error) {
  std::ifstream file(path);
  if (!file) {
    if (error) *error = "no config file at " + path;
    return false;
  }

  std::string line;
  while (std::getline(file, line)) {
    const size_t comment = line.find_first_of("#;");
    if (comment != std::string::npos) line = line.substr(0, comment);
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;

    const std::string key = Trim(line.substr(0, equals));
    const std::string value = Trim(line.substr(equals + 1));
    if (key.empty()) continue;

    if (key == "device_name") {
      device_name = value;
    } else if (key == "speed_axis") {
      speed_axis = atoi(value.c_str());
    } else if (key == "sprint_button") {
      sprint_button = atoi(value.c_str());
    } else if (key == "invert_forward") {
      invert_forward = atoi(value.c_str()) != 0;
    } else if (key == "deadzone") {
      deadzone = static_cast<float>(atof(value.c_str()));
    } else if (key == "max_input") {
      max_input = static_cast<float>(atof(value.c_str()));
    } else if (key == "curve_exponent") {
      curve_exponent = static_cast<float>(atof(value.c_str()));
    } else if (key == "sprint_source") {
      sprint_source = atoi(value.c_str()) == 1
                          ? SprintSource::kSoftwareThreshold
                          : SprintSource::kHardwareButton;
    } else if (key == "sprint_threshold") {
      sprint_threshold = static_cast<float>(atof(value.c_str()));
    } else if (key == "fallback_enabled") {
      fallback_enabled = atoi(value.c_str()) != 0;
    } else if (key == "fallback_threshold") {
      fallback_threshold = static_cast<float>(atof(value.c_str()));
    } else if (key == "fallback_hold_ms") {
      fallback_hold_ms = atoi(value.c_str());
    } else if (key == "fallback_forward_key") {
      fallback_forward_key = atoi(value.c_str());
    } else if (key == "fallback_back_key") {
      fallback_back_key = atoi(value.c_str());
    } else if (key == "fallback_sprint_key") {
      fallback_sprint_key = atoi(value.c_str());
    } else if (key == "fallback_require_focus") {
      fallback_require_focus = atoi(value.c_str()) != 0;
    } else if (key == "fallback_target_process") {
      fallback_target_process = value;
    }
  }

  // A zero max_input would divide the whole pipeline by zero.
  if (max_input < 0.01f) max_input = 0.01f;
  if (curve_exponent < 0.1f) curve_exponent = 0.1f;

  if (error) error->clear();
  return true;
}

bool Config::Save(const std::string& path, std::string* error) const {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);

  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    if (error) *error = "could not write " + path;
    return false;
  }

  file << "# conVR treadmill companion settings\n"
       << "# Same format on Linux and Windows: copy this file across to carry\n"
       << "# your tuning between machines.\n"
       << "device_name=" << device_name << "\n"
       << "speed_axis=" << speed_axis << "\n"
       << "sprint_button=" << sprint_button << "\n"
       << "invert_forward=" << (invert_forward ? 1 : 0) << "\n"
       << "deadzone=" << deadzone << "\n"
       << "max_input=" << max_input << "\n"
       << "curve_exponent=" << curve_exponent << "\n"
       << "# sprint_source: 0 = hardware button, 1 = software speed threshold\n"
       << "sprint_source=" << static_cast<int>(sprint_source) << "\n"
       << "sprint_threshold=" << sprint_threshold << "\n"
       << "\n"
       << "# Keyboard fallback for games that cannot read the treadmill through\n"
       << "# SteamVR (legacy-input titles). Off unless you need it. Keys are\n"
       << "# set-1 scancodes: W=17 S=31 LShift=42 LAlt=56 LCtrl=29.\n"
       << "fallback_enabled=" << (fallback_enabled ? 1 : 0) << "\n"
       << "fallback_threshold=" << fallback_threshold << "\n"
       << "# How long the key stays down after the belt drops below threshold.\n"
       << "# Bridges the dip between footfalls; too low stutters, too high\n"
       << "# keeps walking after you stop.\n"
       << "fallback_hold_ms=" << fallback_hold_ms << "\n"
       << "fallback_forward_key=" << fallback_forward_key << "\n"
       << "fallback_back_key=" << fallback_back_key << "\n"
       << "fallback_sprint_key=" << fallback_sprint_key << "\n"
       << "# With require_focus off, the treadmill types into whatever window\n"
       << "# happens to be focused. Leave it on unless you know why not.\n"
       << "fallback_require_focus=" << (fallback_require_focus ? 1 : 0) << "\n"
       << "fallback_target_process=" << fallback_target_process << "\n";

  if (!file.good()) {
    if (error) *error = "write failed for " + path;
    return false;
  }
  if (error) error->clear();
  return true;
}

}  // namespace convr
