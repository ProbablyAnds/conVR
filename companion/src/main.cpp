// conVR treadmill companion: reads the treadmill's HID gamepad through SDL2,
// shapes the belt-speed axis into a joystick value, and streams it to the
// SteamVR driver. The UI doubles as the calibration tool — every raw axis and
// button is on screen so you can see which one the belt actually drives.

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "convr_ipc.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

namespace {

constexpr int kTargetHz = 100;
constexpr double kFramePeriod = 1.0 / kTargetHz;
constexpr float kAxisScale = 32767.0f;

struct DeviceEntry {
  int device_index = -1;
  std::string name;
};

struct Output {
  float x = 0.0f;
  float y = 0.0f;
  bool button = false;
  float raw_normalized = 0.0f;  // after invert, before deadzone/scale/curve
};

std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return text;
}

std::vector<DeviceEntry> EnumerateDevices() {
  std::vector<DeviceEntry> devices;
  const int count = SDL_NumJoysticks();
  devices.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const char* name = SDL_JoystickNameForIndex(i);
    devices.push_back({i, name ? name : "(unnamed device)"});
  }
  return devices;
}

// Belt axis -> joystick Y. Kept as one function so the UI can show exactly
// what the driver is being sent.
Output ComputeOutput(const convr::Config& config, SDL_Joystick* joystick) {
  Output out;
  if (joystick == nullptr) return out;

  const int axis_count = SDL_JoystickNumAxes(joystick);
  if (config.speed_axis < 0 || config.speed_axis >= axis_count) return out;

  float value = SDL_JoystickGetAxis(joystick, config.speed_axis) / kAxisScale;
  value = std::clamp(value, -1.0f, 1.0f);
  if (config.invert_forward) value = -value;
  out.raw_normalized = value;

  const float magnitude = std::fabs(value);
  const float sign = value < 0.0f ? -1.0f : 1.0f;

  // Deadzone, rescaled so the usable range still reaches 1.0.
  float shaped = 0.0f;
  if (magnitude > config.deadzone) {
    const float span = std::max(1.0f - config.deadzone, 1e-4f);
    shaped = (magnitude - config.deadzone) / span;
  }

  // max_input is the normalized reading that should mean "full deflection".
  shaped = shaped / std::max(config.max_input, 0.01f);
  shaped = std::clamp(shaped, 0.0f, 1.0f);

  if (config.curve_exponent != 1.0f) {
    shaped = std::pow(shaped, config.curve_exponent);
  }

  out.x = 0.0f;
  out.y = sign * shaped;

  const int button_count = SDL_JoystickNumButtons(joystick);
  const bool hardware_button =
      config.sprint_button >= 0 && config.sprint_button < button_count &&
      SDL_JoystickGetButton(joystick, config.sprint_button) != 0;

  out.button = config.sprint_source == convr::SprintSource::kHardwareButton
                   ? hardware_button
                   : out.y >= config.sprint_threshold;
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = convr::DefaultConfigPath();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      SDL_Log("usage: convr_companion [--config <path>]");
      SDL_Log("default config: %s", config_path.c_str());
      return 0;
    }
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  // Keep reading the treadmill while the user is looking at the game, not
  // at this window.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

  SDL_Window* window = SDL_CreateWindow(
      "conVR Treadmill Companion", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, 700, 860,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (window == nullptr) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  // No vsync: the send loop paces itself at 100 Hz and must not be throttled
  // to the monitor refresh rate.
  SDL_Renderer* renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (renderer == nullptr) {
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  }
  if (renderer == nullptr) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  // One fixed full-window layout: there is nothing worth persisting, and
  // without this ImGui drops an imgui.ini into whatever directory you ran from.
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  convr::Config config;
  std::string status_message;
  std::string load_error;
  if (config.Load(config_path, &load_error)) {
    status_message = "Loaded " + config_path;
  } else {
    status_message = load_error + " (using defaults)";
  }

  convr::IpcClient ipc;
  std::vector<DeviceEntry> devices = EnumerateDevices();
  SDL_Joystick* joystick = nullptr;
  std::string open_device_name;
  bool auto_connect = true;
  uint64_t packets_sent = 0;
  uint64_t send_failures = 0;

  double next_ipc_retry = 0.0;
  double next_device_retry = 0.0;
  auto now_seconds = [] {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
  };

  auto open_device = [&](int device_index) {
    if (joystick != nullptr) {
      SDL_JoystickClose(joystick);
      joystick = nullptr;
      open_device_name.clear();
    }
    if (device_index < 0 || device_index >= SDL_NumJoysticks()) return;
    joystick = SDL_JoystickOpen(device_index);
    if (joystick != nullptr) {
      const char* name = SDL_JoystickName(joystick);
      open_device_name = name ? name : "(unnamed device)";
    }
  };

  // Substring match so the configured "conVR Treadmill Controller" still finds
  // the device when the OS prefixes it with the USB vendor string.
  auto find_configured_device = [&]() -> int {
    if (config.device_name.empty()) return -1;
    const std::string needle = ToLower(config.device_name);
    for (const DeviceEntry& device : devices) {
      if (ToLower(device.name).find(needle) != std::string::npos) {
        return device.device_index;
      }
    }
    return -1;
  };

  const int matched = find_configured_device();
  if (matched >= 0) open_device(matched);

  bool running = true;
  while (running) {
    const double frame_start = now_seconds();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) running = false;
      if (event.type == SDL_WINDOWEVENT &&
          event.window.event == SDL_WINDOWEVENT_CLOSE &&
          event.window.windowID == SDL_GetWindowID(window)) {
        running = false;
      }
      if (event.type == SDL_JOYDEVICEADDED || event.type == SDL_JOYDEVICEREMOVED) {
        devices = EnumerateDevices();
      }
    }

    // --- device health ---
    if (joystick != nullptr && !SDL_JoystickGetAttached(joystick)) {
      SDL_JoystickClose(joystick);
      joystick = nullptr;
      open_device_name.clear();
    }
    if (joystick == nullptr && auto_connect && frame_start >= next_device_retry) {
      next_device_retry = frame_start + 1.0;
      devices = EnumerateDevices();
      const int index = find_configured_device();
      if (index >= 0) open_device(index);
    }

    // --- driver link health ---
    if (!ipc.connected() && frame_start >= next_ipc_retry) {
      next_ipc_retry = frame_start + 1.0;
      ipc.Connect();
    }

    const Output output = ComputeOutput(config, joystick);

    if (ipc.connected()) {
      convr::TreadmillPacket packet = {};
      packet.magic = convr::kProtocolMagic;
      packet.joystick_x = output.x;
      packet.joystick_y = output.y;
      packet.button = output.button ? 1 : 0;
      if (ipc.Send(packet)) {
        ++packets_sent;
      } else {
        ++send_failures;
      }
    }

    // --- UI ---
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("conVR", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    const ImVec4 kGood(0.35f, 0.85f, 0.45f, 1.0f);
    const ImVec4 kBad(0.95f, 0.45f, 0.40f, 1.0f);
    const ImVec4 kDim(0.60f, 0.60f, 0.60f, 1.0f);

    // ---- Device ----
    if (ImGui::CollapsingHeader("Device", ImGuiTreeNodeFlags_DefaultOpen)) {
      const char* preview = joystick != nullptr ? open_device_name.c_str()
                                                : "(no device open)";
      if (ImGui::BeginCombo("Treadmill device", preview)) {
        for (const DeviceEntry& device : devices) {
          const bool selected = (device.name == open_device_name);
          if (ImGui::Selectable(device.name.c_str(), selected)) {
            config.device_name = device.name;
            open_device(device.device_index);
          }
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (ImGui::Button("Rescan")) devices = EnumerateDevices();

      ImGui::Checkbox("Reopen automatically by name", &auto_connect);

      if (joystick != nullptr) {
        ImGui::TextColored(kGood, "Connected: %d axes, %d buttons",
                           SDL_JoystickNumAxes(joystick),
                           SDL_JoystickNumButtons(joystick));
      } else if (devices.empty()) {
        ImGui::TextColored(kBad, "No joysticks detected at all.");
      } else {
        ImGui::TextColored(kBad, "Not open. Pick a device above.");
      }
    }

    // ---- Driver link ----
    if (ImGui::CollapsingHeader("SteamVR driver link",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      if (ipc.connected()) {
        ImGui::TextColored(kGood, "Connected - %llu packets sent",
                           static_cast<unsigned long long>(packets_sent));
      } else {
        ImGui::TextColored(kBad, "Not connected (retrying every second)");
        ImGui::TextColored(kDim, "SteamVR must be running with the convr "
                                 "driver enabled.");
      }
      ImGui::TextColored(kDim, "Endpoint: %s",
                         convr::EndpointPath(convr::kEndpointName).c_str());
      if (send_failures > 0) {
        ImGui::TextColored(kDim, "Dropped links: %llu  last error: %s",
                           static_cast<unsigned long long>(send_failures),
                           ipc.last_error().c_str());
      }
    }

    // ---- Live input ----
    if (ImGui::CollapsingHeader("Live input (identify your axis and button)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      if (joystick == nullptr) {
        ImGui::TextColored(kDim, "Open a device to see live values.");
      } else {
        ImGui::TextColored(kDim,
                           "Walk on the belt and watch which axis moves.");
        const int axis_count = SDL_JoystickNumAxes(joystick);
        for (int axis = 0; axis < axis_count; ++axis) {
          const int raw = SDL_JoystickGetAxis(joystick, axis);
          const float normalized = std::clamp(raw / kAxisScale, -1.0f, 1.0f);
          const bool is_speed_axis = axis == config.speed_axis;

          char label[64];
          snprintf(label, sizeof(label), "axis %d%s", axis,
                   is_speed_axis ? " (belt speed)" : "");
          if (is_speed_axis) ImGui::PushStyleColor(ImGuiCol_Text, kGood);
          ImGui::Text("%-22s %7d", label, raw);
          if (is_speed_axis) ImGui::PopStyleColor();
          ImGui::SameLine();
          // Map -1..1 onto 0..1 so the bar centres when the belt is still.
          ImGui::ProgressBar(normalized * 0.5f + 0.5f, ImVec2(-1.0f, 0.0f),
                             "");
        }

        ImGui::Spacing();
        const int button_count = SDL_JoystickNumButtons(joystick);
        for (int button = 0; button < button_count; ++button) {
          const bool pressed = SDL_JoystickGetButton(joystick, button) != 0;
          const bool is_sprint = button == config.sprint_button;
          if (!pressed && !is_sprint) continue;  // keep 32 buttons readable
          ImGui::TextColored(pressed ? kGood : kDim, "button %2d: %s%s", button,
                             pressed ? "PRESSED" : "released",
                             is_sprint ? "   (sprint)" : "");
        }
        ImGui::TextColored(kDim,
                           "(only the sprint button and any pressed buttons "
                           "are listed)");
      }
    }

    // ---- Output ----
    if (ImGui::CollapsingHeader("Sent to SteamVR",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Text("joystick x %+.3f", output.x);
      ImGui::ProgressBar(output.x * 0.5f + 0.5f, ImVec2(-1.0f, 0.0f), "");
      ImGui::Text("joystick y %+.3f  %s", output.y,
                  output.y > 0.01f    ? "(forward)"
                  : output.y < -0.01f ? "(backward)"
                                      : "(centred)");
      ImGui::ProgressBar(output.y * 0.5f + 0.5f, ImVec2(-1.0f, 0.0f), "");
      ImGui::TextColored(output.button ? kGood : kDim, "sprint button: %s",
                         output.button ? "PRESSED" : "released");
      ImGui::TextColored(kDim, "axis after invert, before shaping: %+.3f",
                         output.raw_normalized);
    }

    // ---- Settings ----
    if (ImGui::CollapsingHeader("Tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
      const int axis_count =
          joystick != nullptr ? SDL_JoystickNumAxes(joystick) : 8;
      const int button_count =
          joystick != nullptr ? SDL_JoystickNumButtons(joystick) : 32;

      ImGui::SliderInt("Belt speed axis", &config.speed_axis, 0,
                       std::max(axis_count - 1, 0));
      ImGui::SliderInt("Sprint button", &config.sprint_button, 0,
                       std::max(button_count - 1, 0));
      ImGui::Checkbox("Invert forward", &config.invert_forward);
      ImGui::SetItemTooltip(
          "The firmware drives the axis negative when walking forward; "
          "SteamVR wants positive Y forward.");

      ImGui::SliderFloat("Deadzone", &config.deadzone, 0.0f, 0.5f, "%.3f");
      ImGui::SliderFloat("Full deflection at", &config.max_input, 0.05f, 1.0f,
                         "%.3f");
      ImGui::SetItemTooltip(
          "Normalized axis reading that should mean full stick. Lower this if "
          "you never reach full speed in game.");
      ImGui::SliderFloat("Response curve", &config.curve_exponent, 0.5f, 3.0f,
                         "%.2f");
      ImGui::SetItemTooltip("1.0 is linear. Above 1.0 softens the low end.");

      int sprint_source = static_cast<int>(config.sprint_source);
      ImGui::RadioButton("Sprint from hardware button", &sprint_source, 0);
      ImGui::SameLine();
      ImGui::RadioButton("Sprint from speed", &sprint_source, 1);
      config.sprint_source = static_cast<convr::SprintSource>(sprint_source);
      if (config.sprint_source == convr::SprintSource::kSoftwareThreshold) {
        ImGui::SliderFloat("Sprint threshold", &config.sprint_threshold, 0.0f,
                           1.0f, "%.2f");
      }
    }

    // ---- Config file ----
    if (ImGui::CollapsingHeader("Config file",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::TextWrapped("%s", config_path.c_str());
      if (ImGui::Button("Save")) {
        std::string error;
        status_message = config.Save(config_path, &error)
                             ? "Saved " + config_path
                             : "Save failed: " + error;
      }
      ImGui::SameLine();
      if (ImGui::Button("Reload")) {
        std::string error;
        status_message = config.Load(config_path, &error)
                             ? "Reloaded " + config_path
                             : "Reload failed: " + error;
        if (auto_connect) {
          const int index = find_configured_device();
          if (index >= 0) open_device(index);
        }
      }
      ImGui::TextColored(kDim, "%s", status_message.c_str());
    }

    ImGui::End();

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 24, 24, 28, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);

    // --- pace to ~100 Hz ---
    const double elapsed = now_seconds() - frame_start;
    if (elapsed < kFramePeriod) {
      std::this_thread::sleep_for(
          std::chrono::duration<double>(kFramePeriod - elapsed));
    }
  }

  ipc.Disconnect();
  if (joystick != nullptr) SDL_JoystickClose(joystick);
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
