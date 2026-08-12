#include "device_provider.h"

namespace convr {

vr::EVRInitError DeviceProvider::Init(vr::IVRDriverContext* driver_context) {
  VR_INIT_SERVER_DRIVER_CONTEXT(driver_context);

  if (!ipc_.Start()) {
    vr::VRDriverLog()->Log(
        "[convr] FATAL: could not open the IPC endpoint; is another vrserver "
        "still running?");
    return vr::VRInitError_Driver_Failed;
  }
  vr::VRDriverLog()->Log(
      ("[convr] listening for the companion on " + EndpointPath(kEndpointName))
          .c_str());

  device_ = std::make_unique<TreadmillDevice>();
  if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
          device_->serial_number(), vr::TrackedDeviceClass_Controller,
          device_.get())) {
    vr::VRDriverLog()->Log("[convr] FATAL: TrackedDeviceAdded failed");
    return vr::VRInitError_Driver_Failed;
  }

  return vr::VRInitError_None;
}

void DeviceProvider::Cleanup() {
  ipc_.Stop();
  device_.reset();
  VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

const char* const* DeviceProvider::GetInterfaceVersions() {
  return vr::k_InterfaceVersions;
}

void DeviceProvider::RunFrame() {
  if (device_) device_->RunFrame(ipc_);
}

bool DeviceProvider::ShouldBlockStandbyMode() { return false; }

void DeviceProvider::EnterStandby() {}

void DeviceProvider::LeaveStandby() {}

}  // namespace convr
