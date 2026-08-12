// The single tracked device this driver registers: an input-only controller
// carrying the treadmill's locomotion role. It has no pose of its own — the
// player's real motion controllers keep tracking their hands — so it reports a
// static identity pose purely to stay "connected".
#pragma once

#include <openvr_driver.h>

#include "convr_ipc.h"

namespace convr {

class TreadmillDevice : public vr::ITrackedDeviceServerDriver {
 public:
  TreadmillDevice();
  // Not `override`: OpenVR's interfaces have non-virtual destructors. SteamVR
  // never deletes us through a base pointer - the provider owns this object.
  ~TreadmillDevice() = default;

  // ITrackedDeviceServerDriver
  vr::EVRInitError Activate(uint32_t object_id) override;
  void Deactivate() override;
  void EnterStandby() override;
  void* GetComponent(const char* component_name_and_version) override;
  void DebugRequest(const char* request, char* response_buffer,
                    uint32_t response_buffer_size) override;
  vr::DriverPose_t GetPose() override;

  // Called once per SteamVR frame by the provider.
  void RunFrame(const IpcServer& ipc);

  const char* serial_number() const { return kSerialNumber; }

 private:
  static constexpr const char* kSerialNumber = "convr-treadmill-1";

  uint32_t object_id_ = vr::k_unTrackedDeviceIndexInvalid;
  vr::PropertyContainerHandle_t property_container_ =
      vr::k_ulInvalidPropertyContainer;

  vr::VRInputComponentHandle_t joystick_x_ = vr::k_ulInvalidInputComponentHandle;
  vr::VRInputComponentHandle_t joystick_y_ = vr::k_ulInvalidInputComponentHandle;
  vr::VRInputComponentHandle_t button_ = vr::k_ulInvalidInputComponentHandle;

  // Mirrors of the last values pushed, so the log only reports real changes.
  float last_x_ = 0.0f;
  float last_y_ = 0.0f;
  bool last_button_ = false;
  bool last_companion_connected_ = false;
};

}  // namespace convr
