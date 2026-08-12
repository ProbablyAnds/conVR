// The one exported symbol SteamVR looks for when it loads driver_convr.
#include <openvr_driver.h>

#include <string.h>

#include "device_provider.h"

#if defined(_WIN32)
#define CONVR_EXPORT extern "C" __declspec(dllexport)
#else
#define CONVR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
convr::DeviceProvider g_device_provider;
}

CONVR_EXPORT void* HmdDriverFactory(const char* interface_name,
                                    int* return_code) {
  if (strcmp(interface_name, vr::IServerTrackedDeviceProvider_Version) == 0) {
    return &g_device_provider;
  }

  // Anything else (the watchdog provider in particular) is not ours; SteamVR
  // expects the "not found" code rather than a null with no explanation.
  if (return_code) *return_code = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
