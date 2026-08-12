#pragma once

#include <openvr_driver.h>

#include <memory>

#include "convr_ipc.h"
#include "treadmill_device.h"

namespace convr {

class DeviceProvider : public vr::IServerTrackedDeviceProvider {
 public:
  DeviceProvider() = default;
  // See TreadmillDevice: OpenVR interfaces have non-virtual destructors.
  ~DeviceProvider() = default;

  vr::EVRInitError Init(vr::IVRDriverContext* driver_context) override;
  void Cleanup() override;
  const char* const* GetInterfaceVersions() override;
  void RunFrame() override;
  bool ShouldBlockStandbyMode() override;
  void EnterStandby() override;
  void LeaveStandby() override;

 private:
  std::unique_ptr<TreadmillDevice> device_;
  IpcServer ipc_;
};

}  // namespace convr
