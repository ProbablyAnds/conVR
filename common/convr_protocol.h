// Wire format shared by the companion (sender) and the SteamVR driver
// (receiver). Fixed-size little-endian frames; both ends are built from this
// same header so there is no versioning dance beyond the magic field.
#pragma once

#include <stdint.h>

namespace convr {

// Bump if the layout of TreadmillPacket ever changes. The driver drops frames
// whose magic does not match, so a stale companion cannot feed it garbage.
constexpr uint32_t kProtocolMagic = 0x31525643;  // 'CVR1' little-endian

// The IPC endpoint name. Expanded to \\.\pipe\convr_treadmill on Windows and
// to a socket under the user runtime directory on Linux; see convr_ipc.cpp.
constexpr const char* kEndpointName = "convr_treadmill";

#pragma pack(push, 1)
struct TreadmillPacket {
  uint32_t magic;
  float joystick_x;  // -1.0 .. 1.0
  float joystick_y;  // -1.0 .. 1.0, positive is forward
  uint8_t button;    // 0 or 1
  uint8_t reserved[3];
};
#pragma pack(pop)

static_assert(sizeof(TreadmillPacket) == 16, "packet layout must be stable");

}  // namespace convr
