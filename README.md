# conVR

A VR treadmill that drives locomotion in SteamVR while you keep your real
motion controllers for your hands.

Three pieces:

| Piece | What it is | Where |
| --- | --- | --- |
| Firmware | Pico 2 W reads a quadrature encoder on the belt and enumerates as a USB HID gamepad | `conVR.ino` |
| Driver | SteamVR driver registering one device with the treadmill locomotion role | `driver/` |
| Companion | SDL2 + Dear ImGui app that reads the gamepad, shapes the signal, and feeds the driver | `companion/` |

The companion talks to the driver over a local IPC link (named pipe on Windows,
Unix domain socket on Linux) behind one small shim in `common/`. Develop and
tune on Linux, copy the config file to Windows, play there. No XOutput, no
ViGEmBus.

---

# PC software

## Building

One CMake project builds both the driver and the companion on both operating
systems.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

Outputs:

- `build/convr/` — the driver folder, laid out exactly as SteamVR wants it
- `build/bin/convr_companion` (`.exe` on Windows)

### Dependencies

| Dependency | How it is resolved |
| --- | --- |
| OpenVR | `openvr_driver.h` alone is downloaded at configure time (pinned to `v2.5.1`, SHA-256 checked). The driver needs headers only — it never links `openvr_api`. |
| SDL2 | System SDL2 if `find_package`/pkg-config finds one, otherwise fetched and built. |
| Dear ImGui | Fetched at `v1.91.5` and compiled into a small static library. |

Offline or air-gapped builds: pass `-DCONVR_OPENVR_INCLUDE_DIR=/path/to/openvr/headers`
to skip the download.

### Linux

Needs a C++17 compiler, CMake 3.16+, and SDL2 development headers.

```
sudo pacman -S base-devel cmake sdl2          # Arch
sudo apt install build-essential cmake libsdl2-dev   # Debian/Ubuntu
```

### Windows

Needs Visual Studio 2019+ (Desktop C++ workload) and CMake. SDL2 is fetched
automatically, so there is nothing to install by hand.

```
cmake -S . -B build
cmake --build build --config Release
```

`SDL2.dll` is copied next to `convr_companion.exe` automatically.

## Installing the driver

```
cmake --install build --component driver
```

This copies `build/convr/` into the SteamVR drivers directory:

| OS | Path |
| --- | --- |
| Linux | `~/.steam/steam/steamapps/common/SteamVR/drivers/convr` |
| Windows | `C:\Program Files (x86)\Steam\steamapps\common\SteamVR\drivers\convr` |

If Steam lives somewhere else, point the build at it:

```
cmake -S . -B build -DCONVR_STEAMVR_DRIVERS_DIR="D:/SteamLibrary/steamapps/common/SteamVR/drivers"
```

The installed folder looks like this:

```
drivers/convr/
  driver.vrdrivermanifest
  bin/linux64/driver_convr.so        (or bin/win64/driver_convr.dll)
  resources/input/convr_treadmill_profile.json
  resources/input/legacy_bindings_convr_treadmill.json
  resources/settings/default.vrsettings
```

Re-run `cmake --install` after every rebuild, and restart SteamVR to pick up a
new driver binary.

## Enabling it in SteamVR

The driver ships `resources/settings/default.vrsettings` with `enable: true`, so
it usually just works. **One setting you almost certainly do need** is
`activateMultipleDrivers`, which lets a third-party driver run alongside your
HMD's driver. Without it SteamVR may load only the driver that provides the
headset.

Edit `steamvr.vrsettings` while SteamVR is **closed**:

| OS | Path |
| --- | --- |
| Linux | `~/.steam/steam/config/steamvr.vrsettings` |
| Windows | `C:\Program Files (x86)\Steam\config\steamvr.vrsettings` |

```json
{
   "steamvr": {
      "activateMultipleDrivers": true
   },
   "driver_convr": {
      "enable": true
   }
}
```

Merge those keys into the existing JSON — do not replace the file.

## Running the companion

```
./build/bin/convr_companion            # Linux
build\bin\Release\convr_companion.exe  # Windows
```

Optional: `--config <path>` to use a config file somewhere other than the
default.

The window has five sections:

- **Device** — dropdown of every SDL joystick it can see, plus a Rescan button.
  It reopens the treadmill automatically by name, so unplugging and replugging
  mid-session recovers on its own.
- **SteamVR driver link** — whether the IPC link to the driver is up, the
  endpoint path, and a packet counter. It retries once a second, in either
  order: you can start the companion before or after SteamVR.
- **Live input** — the raw value of every axis and every pressed button. This is
  how you work out which axis is belt speed and which button is sprint: walk on
  the belt and watch which bar moves.
- **Sent to SteamVR** — the final `{x, y, button}` going to the driver.
- **Tuning** — settings below, applied live.
- **Config file** — Save / Reload, with the file path shown.

### Tuning settings

| Setting | Meaning |
| --- | --- |
| Belt speed axis | SDL axis index carrying belt speed |
| Sprint button | SDL button index for the firmware's sprint button |
| Invert forward | Firmware drives the axis negative when walking forward; SteamVR wants positive-Y forward, so this defaults **on** |
| Deadzone | Normalized; kills idle jitter around centre |
| Full deflection at | The normalized axis reading that should mean full stick. Lower it if you never reach full speed in game |
| Response curve | 1.0 linear; above 1.0 softens the low end |
| Sprint source | Hardware button (default) or a software threshold on output speed |

### What this device actually reports

Measured on the real controller, so you know what "correct" looks like before
you start tuning:

```
name:  Raspberry Pi conVR Treadmill Controller
axes:  6      buttons: 32     hats: 1
idle:  every axis reads -1 (i.e. centred) with the belt stopped
```

The USB name carries a `Raspberry Pi ` vendor prefix, which is why the
companion matches `device_name` as a **case-insensitive substring** — the
default `conVR Treadmill Controller` finds it without you typing the prefix.

The firmware numbers its HID buttons from 1; SDL numbers them from 0. That
off-by-one is the single most likely reason a button "does nothing":

| Firmware (`conVR.ino`) | HID button | SDL index to enter in the companion |
| --- | --- | --- |
| `Joystick.position(0, axis)` — belt speed on left-stick Y | axis Y | **axis 1** |
| `Joystick.button(1, buttonPressed)` — the GP4 pushbutton | 1 | **button 0** |
| `Joystick.button(SPRINT_BUTTON, sprintActive)` — auto-sprint | 9 | **button 8** |

An axis stuck at `-32768` rather than `-1` is one the firmware never reports —
it is not your belt axis. Always confirm by walking: the belt axis is the one
whose bar moves.

### Two sprint mechanisms — pick one

Sprint can come from either end of the chain, and running both at once gives
you two thresholds fighting each other:

- **Firmware auto-sprint** (`SPRINT_START_SPEED_MPS` / `SPRINT_RELEASE_SPEED_MPS`
  in `conVR.ino`, with hysteresis) arrives as HID button 9 → **SDL button 8**.
  Use it by setting *Sprint button* to 8 and *Sprint source* to hardware.
- **Companion software threshold** (*Sprint from speed*) fires on the shaped
  output value instead, and is retunable without reflashing.

Default configuration uses the hardware button at SDL index 0 — the physical
pushbutton. Change *Sprint button* to 8 if you want the firmware's automatic
speed-triggered sprint instead.

### Config file

Plain INI, identical format on both operating systems:

| OS | Path |
| --- | --- |
| Linux | `~/.config/convr/companion.ini` (respects `XDG_CONFIG_HOME`) |
| Windows | `%APPDATA%\convr\companion.ini` |

Tune on Linux, copy that file to the Windows path, and your calibration comes
with you.

## Verifying on Linux (no game required)

1. Start SteamVR.
2. Start the companion. **Device** should show your treadmill with its axis and
   button count; **SteamVR driver link** should go green with a rising packet
   count.
3. Walk on the belt. The **Live input** bar for your speed axis should move, and
   **Sent to SteamVR** `joystick y` should go positive as you walk forward. If
   it goes negative, toggle *Invert forward*.
4. In the SteamVR window: **Devices → Manage Trackers**, or hover the status
   window — the treadmill appears as an extra connected device named
   *conVR Treadmill Controller* alongside your HMD and controllers.
5. Deeper check: launch SteamVR with `CONVR_DEBUG=1` set, and the driver logs a
   line every couple of seconds to `~/.steam/steam/logs/vrserver.txt`:

   ```
   convr: [convr] rx packets=914 x=+0.000 y=+0.184 button=1
   ```

   `grep convr ~/.steam/steam/logs/vrserver.txt` also shows the driver loading,
   the device activating, and the companion connecting or disconnecting.

## Developing and testing without an HMD

The whole PC side can be exercised headlessly on Linux, which is how it was
built. The one thing that trips everyone up:

> **vrserver only loads drivers once an OpenVR client connects.** Starting
> vrserver on its own and finding no `convr` lines in the log proves nothing —
> the driver was never asked for.

### 1. Point SteamVR at the null HMD

Back up `~/.steam/steam/config/steamvr.vrsettings` first, and restore it when
you are done — these settings will stop your real headset working.

```json
{
   "steamvr": {
      "requireHmd": false,
      "forcedDriver": "null",
      "activateMultipleDrivers": true
   },
   "driver_null": { "enable": true },
   "driver_convr": { "enable": true }
}
```

In a test script, restore it from a shell trap so a failure cannot leave your
config broken:

```bash
trap 'cp "$BACKUP" "$CFG"' EXIT
```

### 2. Start vrserver

```bash
SVR=~/.steam/steam/steamapps/common/SteamVR
: > ~/.steam/steam/logs/vrserver.txt          # start from a clean log
CONVR_DEBUG=1 LD_LIBRARY_PATH="$SVR/bin/linux64" "$SVR/bin/linux64/vrserver" &
```

`CONVR_DEBUG=1` must be set **on vrserver**, not on the companion — the driver
reads it from the process it is loaded into.

### 3. Connect a client to trigger driver loading

`vrcmd` core-dumps on this setup. A tiny background app works and doubles as
the read-back check — this is the whole program:

```cpp
// g++ -std=c++17 -I<openvr>/headers verify.cpp -o verify \
//   -L$SVR/bin/linux64 -lopenvr_api -Wl,-rpath,$SVR/bin/linux64
#include <openvr.h>
#include <stdio.h>
#include <thread>
int main() {
  vr::EVRInitError e{};
  vr::IVRSystem* vrs = vr::VR_Init(&e, vr::VRApplication_Background);
  if (!vrs) { printf("%s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return 1; }
  std::this_thread::sleep_for(std::chrono::seconds(2));
  for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
    if (!vrs->IsTrackedDeviceConnected(i)) continue;
    char type[128]{};
    vrs->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type, nullptr);
    printf("[%2u] class=%d role=%d type=%s\n", i, (int)vrs->GetTrackedDeviceClass(i),
           vrs->GetInt32TrackedDeviceProperty(i, vr::Prop_ControllerRoleHint_Int32, nullptr), type);
  }
  std::this_thread::sleep_for(std::chrono::seconds(30));   // hold the session open
  vr::VR_Shutdown();
}
```

A healthy run prints the treadmill with role `4`
(`TrackedControllerRole_Treadmill`):

```
[ 0] class=1 role=0 type=null_hmd
[ 1] class=2 role=4 type=convr_treadmill
```

### 4. Read the log

```bash
grep convr ~/.steam/steam/logs/vrserver.txt
```

```
convr: [convr] listening for the companion on /tmp/convr_treadmill-1000.sock
Loaded server driver convr (IServerTrackedDeviceProvider_004) from .../driver_convr.so
convr: [convr] treadmill device activated
Driver 'convr' finished adding tracked device with serial number 'convr-treadmill-1'
convr: [convr] companion connected
convr: [convr] rx packets=914 x=+0.000 y=+0.184 button=1
```

That last line is the decisive one: it is the driver reporting what it received
from the companion, so if it shows your belt values the whole chain below
SteamVR is healthy.

### Testing without walking on the belt

To exercise the driver with known values, run a client of your own against the
IPC endpoint instead of the companion — include `common/convr_ipc.h`, connect
an `IpcClient`, and stream `TreadmillPacket`s. That is exactly what
`common/ipc_selftest.cpp` does, and it is the quickest way to prove a signal
path without a treadmill attached.

### Gotcha when scripting this

`pkill -f convr_companion` will match **the shell running your script**,
because the pattern appears in that shell's own command line — it kills your
test harness mid-run. Use `pkill -x convr_companion` instead.

## Binding it in Skyrim VR

In the VR dashboard: **Settings → Controllers → Manage Controller Bindings →
Skyrim VR**. Choose the treadmill device (*conVR Treadmill Controller*) from the
device list, then:

- bind its **joystick** to the movement / walk-run action
- bind its **sprint button** to Sprint (Left Joystick Click in Skyrim VR)

Your hand controllers stay bound to everything else — the treadmill is a
separate device with the locomotion role, not a replacement for either hand.

Save the binding; SteamVR remembers it per-application.

### An honest caveat about legacy-input games

Skyrim VR uses OpenVR's **legacy input** API rather than the modern
action-based one. Both halves of what follows were measured on Linux, not
assumed:

- The treadmill's inputs **are** live and bindable through SteamVR's
  action/binding system. A ramp fed in by the companion came back out as a
  bound analog action sweeping the full `-1.000 … +0.995` with correct sign,
  and the sprint button came back as a bound digital action toggling in sync,
  over 200/200 clean update cycles.
- SteamVR does **not** synthesise a legacy per-device controller state for a
  treadmill-role device. `IVRSystem::GetControllerState` on it reads all zeros
  while that same input is arriving correctly through the action API — that
  call only covers hand controllers. A legacy game therefore never polls the
  treadmill directly, and getting it into Skyrim VR depends on the binding UI
  above mapping the treadmill onto the legacy movement input.

**If the treadmill does not move your character in Skyrim VR, start here.** Do
not go hunting for bugs in the driver, the IPC, or the companion: those are
verified working end to end, and the `[convr] rx packets=…` log line will
confirm it in seconds. The question to answer is whether SteamVR's binding UI
will map a treadmill-role device onto a legacy title's movement input.

That last step needs a real HMD and had not been tried at the time of writing.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| Driver link never connects | SteamVR not running, or the driver is not enabled. `grep convr ~/.steam/steam/logs/vrserver.txt`. |
| Driver never loads | `activateMultipleDrivers` is not set to `true`. |
| No `convr` lines in the log **at all** | Nothing has connected to vrserver yet, so it never loaded any drivers. Start a VR app (or the test client above). |
| Companion says connected but nothing moves | Wrong belt-speed axis. Watch **Live input** while walking — the belt axis is the one whose bar moves. |
| An axis sits at `-32768` and never moves | That axis is not reported by the firmware. It is not your belt axis. |
| Sprint button does nothing | Firmware numbers buttons from 1, SDL from 0. The pushbutton is **SDL 0**; firmware auto-sprint is **SDL 8**. |
| You walk forward and the game walks backward | Toggle *Invert forward*. |
| Second companion shows "not connected" | By design — the driver serves one companion at a time and turns away extras cleanly. Close the first one. |
| Stick never reaches full travel | Lower *Full deflection at*. |
| Companion and driver both look healthy, game does nothing | Legacy-input limitation — see the caveat above before debugging anything else. |
| Driver changes have no effect | Re-run `cmake --install` and restart SteamVR; the old `.so`/`.dll` stays loaded otherwise. |

## Layout

```
common/     IPC shim + wire format, shared by both programs
driver/     SteamVR driver (.so / .dll) + manifest and input profile
companion/  SDL2 + Dear ImGui tuning and streaming app
```

The only OS-specific code in the project is inside `common/convr_ipc.cpp`.

### The IPC link

The driver is the server (it outlives the companion), the companion is the
client. Frames are a fixed 16-byte `TreadmillPacket` — magic, `x`, `y`, button
— so there is no parsing, and the reader realigns on the magic if anything ever
goes out of step.

| | Endpoint |
| --- | --- |
| Windows | `\\.\pipe\convr_treadmill` |
| Linux | `/tmp/convr_treadmill-<uid>.sock` |

The Linux path is deliberately derived from your uid rather than from
`XDG_RUNTIME_DIR`: vrserver is often launched from a very different environment
than the companion, and anything environment-dependent risks the two processes
disagreeing about where to meet. Set `CONVR_IPC_PATH` on **both** processes to
override it.

The server runs a background thread doing blocking accept/read and publishes
only the newest packet, so the driver's `RunFrame` never blocks on I/O. It
serves one companion at a time and accepts-then-immediately-closes any extra,
so a second copy gets a clean disconnect it can report and retry from rather
than sends that mysteriously fail.

### Driver interface versions

Built against OpenVR `v2.5.1`, implementing `IServerTrackedDeviceProvider_004`
and `ITrackedDeviceServerDriver_005`, using `IVRDriverInput_003` and
`IVRServerDriverHost_006`. `GetInterfaceVersions()` returns
`vr::k_InterfaceVersions`, so SteamVR checks compatibility for us.

Note that the OpenVR interfaces have **non-virtual destructors**, so the driver
classes deliberately do not mark theirs `override`. Nothing deletes these
objects through a base pointer — the provider owns the device, and the provider
itself is a static.

Run the IPC self-test with:

```
cmake -S . -B build -DCONVR_BUILD_TESTS=ON && cmake --build build
./build/bin/convr_ipc_selftest
```

---

# Firmware

USB game-controller firmware for a VR treadmill belt encoder, built around a
Raspberry Pi Pico 2 W and a CN3806 optical quadrature encoder.

## Hardware

- Raspberry Pi Pico 2 W
- CN3806 open-collector quadrature encoder, 600 PPR
- 75 mm friction wheel
- Two 1 kOhm pull-up resistors
- Normally-open pushbutton

| Encoder wire | Pico connection |
| --- | --- |
| Red | VBUS (5 V) |
| Black | GND |
| Green / phase A | GP2, plus 1 kOhm to 3V3 |
| White / phase B | GP3, plus 1 kOhm to 3V3 |

Connect the pushbutton between GP4 and GND on physical pin 38. The firmware
uses the Pico's internal pull-up and reports it as the first USB HID button,
which can be bound to A in Steam.

The firmware also holds virtual Button 9 (conventional left-stick click) while
belt speed is at least 0.80 m/s and releases it below 0.65 m/s. Bind Button 9
to Skyrim VR's Sprint/Left Joystick Click action. The separate start and
release thresholds prevent sprint from flickering near the transition speed.

Do not pull either encoder output up to 5 V: Pico GPIO is 3.3 V only. The two
external pull-ups are required for reliable operation at treadmill speed.

## Arduino IDE settings

- Board package: **Raspberry Pi Pico/RP2040/RP2350** (`arduino-pico`)
- Board: **Raspberry Pi Pico 2 W**
- USB Stack: **Pico SDK** (not Adafruit TinyUSB)

No additional Arduino library is needed. The `Joystick` library ships with the
arduino-pico core. The Raspberry Pi PIO quadrature program is included in this
repository.

## Controller behaviour

The Pico enumerates as a generic USB HID game controller. Belt velocity drives
the 16-bit left-stick Y axis:

- stopped belt: `0` (centred)
- forward movement: negative Y (stick forward)
- backward movement: positive Y (stick backward)
- 1.4 m/s: full stick travel (calibrated from live walking/running captures)

Edit the constants near the top of `conVR.ino` to change maximum speed,
direction, filtering, or the stop timeout. Set `ENCODER_DIRECTION` to `-1` if
forward movement is reported as a negative speed in the serial monitor.

Serial diagnostics run at 115200 baud and show position, count delta, raw and
filtered belt speed, the resulting joystick value, and button state (`1` when
pressed, `0` when released). The final `sprint` column shows the automatic
sprint-button state.

## Implementation

The official Raspberry Pi quadrature PIO program counts every A/B transition
without CPU interrupts. Firmware samples its signed 32-bit count at 100 Hz,
converts the change to metres per second, applies a low-latency filter, and
sends a USB HID report. A 120 ms no-motion timeout bridges the short pauses
between steps but hard-centres the controller promptly after a real stop.
