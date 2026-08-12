#include "treadmill_device.h"

#include <stdio.h>
#include <stdlib.h>

namespace convr {
namespace {

// If the companion stops sending we must not leave the stick pinned forward,
// or the player keeps walking in-game after the treadmill has stopped.
constexpr double kInputTimeoutSeconds = 0.5;

vr::HmdQuaternion_t IdentityQuaternion() {
  vr::HmdQuaternion_t q;
  q.w = 1.0;
  q.x = 0.0;
  q.y = 0.0;
  q.z = 0.0;
  return q;
}

}  // namespace

TreadmillDevice::TreadmillDevice() = default;

vr::EVRInitError TreadmillDevice::Activate(uint32_t object_id) {
  object_id_ = object_id;
  property_container_ =
      vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id_);

  vr::VRProperties()->SetStringProperty(property_container_,
                                        vr::Prop_ModelNumber_String,
                                        "conVR Treadmill Controller");
  vr::VRProperties()->SetStringProperty(
      property_container_, vr::Prop_SerialNumber_String, kSerialNumber);
  vr::VRProperties()->SetStringProperty(property_container_,
                                        vr::Prop_ManufacturerName_String,
                                        "conVR");
  vr::VRProperties()->SetStringProperty(
      property_container_, vr::Prop_RenderModelName_String, "generic_controller");

  // This is what puts the device alongside the hand controllers rather than
  // replacing one of them: SteamVR treats the treadmill role as a locomotion
  // source, so the user's real controllers keep both hands.
  vr::VRProperties()->SetInt32Property(property_container_,
                                       vr::Prop_ControllerRoleHint_Int32,
                                       vr::TrackedControllerRole_Treadmill);

  // controller_type must match the input profile's "controller_type" field or
  // the bindings UI will not find the profile.
  vr::VRProperties()->SetStringProperty(
      property_container_, vr::Prop_ControllerType_String, "convr_treadmill");
  vr::VRProperties()->SetStringProperty(
      property_container_, vr::Prop_InputProfilePath_String,
      "{convr}/input/convr_treadmill_profile.json");

  vr::VRProperties()->SetBoolProperty(property_container_,
                                      vr::Prop_DeviceIsWireless_Bool, false);
  vr::VRProperties()->SetBoolProperty(property_container_,
                                      vr::Prop_DeviceProvidesBatteryStatus_Bool,
                                      false);
  vr::VRProperties()->SetBoolProperty(property_container_,
                                      vr::Prop_NeverTracked_Bool, true);

  vr::VRDriverInput()->CreateScalarComponent(
      property_container_, "/input/joystick/x", &joystick_x_,
      vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
  vr::VRDriverInput()->CreateScalarComponent(
      property_container_, "/input/joystick/y", &joystick_y_,
      vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
  vr::VRDriverInput()->CreateBooleanComponent(
      property_container_, "/input/sprint/click", &button_);

  vr::VRDriverLog()->Log("[convr] treadmill device activated");
  return vr::VRInitError_None;
}

void TreadmillDevice::Deactivate() {
  object_id_ = vr::k_unTrackedDeviceIndexInvalid;
}

void TreadmillDevice::EnterStandby() {}

void* TreadmillDevice::GetComponent(const char* /*component*/) {
  // Input-only device: no display, camera, or virtual-display components.
  return nullptr;
}

void TreadmillDevice::DebugRequest(const char* /*request*/,
                                   char* response_buffer,
                                   uint32_t response_buffer_size) {
  if (response_buffer_size > 0) response_buffer[0] = '\0';
}

vr::DriverPose_t TreadmillDevice::GetPose() {
  vr::DriverPose_t pose = {};
  pose.poseIsValid = true;
  pose.result = vr::TrackingResult_Running_OK;
  pose.deviceIsConnected = true;
  pose.qWorldFromDriverRotation = IdentityQuaternion();
  pose.qDriverFromHeadRotation = IdentityQuaternion();
  pose.qRotation = IdentityQuaternion();
  // Position stays at the origin; nothing reads it, but SteamVR wants a pose
  // that does not drift or report NaN.
  return pose;
}

void TreadmillDevice::RunFrame(const IpcServer& ipc) {
  if (object_id_ == vr::k_unTrackedDeviceIndexInvalid) return;

  // One line each way, so the log answers "is the companion actually talking
  // to me" without any guessing.
  const bool companion_connected = ipc.client_connected();
  if (companion_connected != last_companion_connected_) {
    vr::VRDriverLog()->Log(companion_connected
                               ? "[convr] companion connected"
                               : "[convr] companion disconnected");
    last_companion_connected_ = companion_connected;
  }

  float x = 0.0f;
  float y = 0.0f;
  bool button = false;

  TreadmillPacket packet;
  double age = 0.0;
  if (ipc.LatestPacket(&packet, &age) && age <= kInputTimeoutSeconds) {
    x = packet.joystick_x;
    y = packet.joystick_y;
    button = packet.button != 0;
  }
  // Anything else (no companion yet, or a stale packet) falls through as a
  // centred stick with the button released.

  if (getenv("CONVR_DEBUG") != nullptr) {
    static int tick = 0;
    if (++tick % 200 == 0) {
      char line[160];
      snprintf(line, sizeof(line),
               "[convr] rx packets=%llu x=%+.3f y=%+.3f button=%d",
               static_cast<unsigned long long>(ipc.packets_received()), x, y,
               button ? 1 : 0);
      vr::VRDriverLog()->Log(line);
    }
  }

  if (x != last_x_) {
    vr::VRDriverInput()->UpdateScalarComponent(joystick_x_, x, 0.0);
    last_x_ = x;
  }
  if (y != last_y_) {
    vr::VRDriverInput()->UpdateScalarComponent(joystick_y_, y, 0.0);
    last_y_ = y;
  }
  if (button != last_button_) {
    vr::VRDriverInput()->UpdateBooleanComponent(button_, button, 0.0);
    last_button_ = button;
  }

  const vr::DriverPose_t pose = GetPose();
  vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, pose,
                                                     sizeof(pose));
}

}  // namespace convr
