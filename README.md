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

Needs the MSVC C++ toolchain and CMake. SDL2 and Dear ImGui are fetched
automatically, so there is nothing else to install by hand. If you have neither
tool yet, `winget` supplies both — the Build Tools install is a few GB and
prompts for elevation:

```
winget install --id Kitware.CMake --scope machine
winget install --id Microsoft.VisualStudio.2022.BuildTools --override ^
  "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Full Visual Studio 2019+ with the Desktop C++ workload works just as well.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Build 64-bit.** SteamVR only ever loads `bin/win64`, and the Visual Studio
2019 generator defaults to Win32 — a 32-bit build stages a DLL that vrserver
then silently refuses to load. `-A x64` is the fix; the build now fails at
configure time rather than letting you find out from an empty log.

Outputs land in `build\bin\` for every configuration, not `build\bin\Release\`.
`SDL2.dll` is copied next to `convr_companion.exe` automatically, and the
companion links as a GUI app, so no console window trails behind it.

Use MSVC rather than MinGW for the driver. SteamVR loads `driver_convr.dll`
across an MSVC-ABI boundary, and GCC orders a hidden struct-return pointer
against `this` the other way round — `GetPose()` returns `DriverPose_t` by
value, so a MinGW build can load and then misbehave in ways that look like a
driver bug rather than an ABI mismatch.

## Installing the driver

On Windows, use the install script. It does the whole job — finds SteamVR from
the registry and `libraryfolders.vdf` (so a SteamVR on a second drive is found
without being told), copies the driver, merges the two settings SteamVR needs
into `steamvr.vrsettings` while preserving everything already in that file, and
takes a timestamped backup first:

```
powershell -ExecutionPolicy Bypass -File scripts\install-windows.ps1
```

It refuses to run while SteamVR is open, because vrserver holds the old
`driver_convr.dll` locked and the install would not take effect until a
restart anyway. It elevates itself only if the drivers directory actually needs
it — a Steam installed under `Program Files (x86)` usually grants its own
folder write access, since Steam self-updates.

The cross-platform equivalent, which copies the driver but does not touch any
settings:

```
cmake --install build --component driver
```

Either way this copies `build/convr/` into the SteamVR drivers directory:

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

On Windows `scripts\install-windows.ps1` already merged both keys for you; this
section is what it did, and what to do by hand elsewhere.

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
./build/bin/convr_companion       # Linux
build\bin\convr_companion.exe     # Windows
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

## Verifying on Windows

The quickest answer comes from the status checker, which walks the whole chain
— device, install, settings, vrserver, driver load, IPC pipe, companion — and
marks each link. The first `[FAIL]` is the one to fix; everything after it
fails as a consequence:

```
powershell -ExecutionPolicy Bypass -File scripts\check-windows.ps1
powershell -ExecutionPolicy Bypass -File scripts\check-windows.ps1 -Follow
```

`-Follow` re-checks every two seconds, so you can watch the links come up while
SteamVR starts. It only reads — it is safe to run at any time.

The rest of this section is what it checks, for when you want to look yourself.
Steps 1-4 above apply unchanged; only the paths differ.

| | Windows |
| --- | --- |
| Driver log | `C:\Program Files (x86)\Steam\logs\vrserver.txt` |
| Endpoint shown in the companion | `\\.\pipe\convr_treadmill` |
| Config file | `%APPDATA%\convr\companion.ini` |

`CONVR_DEBUG=1` has to be set for vrserver, and vrserver is started by Steam
rather than by you, so set it before launching SteamVR:

```
setx CONVR_DEBUG 1        # persistent; new processes only
setx CONVR_DEBUG ""       # undo when you no longer want the logging
```

`setx` only affects processes started afterwards, so **Steam must be fully
exited and restarted** to pick it up — not just SteamVR. If Steam was already
running, the driver still works, you just get no `rx packets=` lines.

Then search the log the same way `grep` does on Linux:

```
Select-String convr "C:\Program Files (x86)\Steam\logs\vrserver.txt"
```

Two checks that need no HMD and take seconds:

- **The IPC link.** `cmake -S . -B build -DCONVR_BUILD_TESTS=ON` then
  `build\bin\convr_ipc_selftest.exe` exercises connect, a 500-packet burst,
  framing, the second-companion refusal, and reconnect over the real named
  pipe. It passes on Windows.
- **The driver binary.** If SteamVR shows no `convr` lines at all, confirm the
  DLL is loadable before looking anywhere else — a DLL that fails to load
  produces no log output to debug from:

  ```
  dumpbin /EXPORTS build\convr\bin\win64\driver_convr.dll
  ```

  It must list `HmdDriverFactory`, and the header must read `machine (x64)`.

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

### The binding image is not decoration

`convr_treadmill_profile.json` must declare **both** `input_bindingui_left` and
`input_bindingui_right`, each pointing at an image under `resources/icons/`.
They look like cosmetic fields and they are not.

SteamVR's binding editor is a CEF page. Its render path reads
`input_bindingui_right.transform` with no null check, and the pose/haptics tabs
read `input_bindingui_left.transform` the same way:

```js
// controllerbindingui.js, verbatim
let n = e.input_bindingui_right.transform ? e.input_bindingui_right.transform : ""
```

With either key missing, that throws `TypeError: Cannot read properties of
undefined (reading 'transform')`, React unwinds the whole tree, and **Manage
Bindings opens as an empty black window** — no error text, nothing in
`vrserver.txt`. The exception shows up only in
`Steam/logs/vrwebhelper_controllerbinding_desktop.txt`. Every controller type
SteamVR ships declares at least `input_bindingui_right`, including the
single-device ones (gamepad, all the Vive trackers), which is why nothing else
trips over it.

`"priority": 5` matters for the same reason. The editor auto-selects a
controller by score, `1000 * (999 - priority) + 10 * deviceCount + …`, so a
device left at the default priority 0 outranks the hand controllers whenever
those are asleep and the editor opens on the treadmill by default. Trackers use
5–6; the treadmill is the same kind of secondary device.

`resources/localization/localization.json` is what makes the device list say
*conVR Treadmill* instead of the raw string `convr_treadmill`.

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

### What a real HMD showed

Two things were confirmed on hardware (Quest 2, Skyrim VR, app 611670):

- The shipped `legacy_bindings_convr_treadmill.json` was **inert**. It named
  `/actions/legacy/in/Axis0`, which does not exist — SteamVR's
  `resources/config/legacy_actions.json` only defines `Left_Axis0_Value`,
  `Right_Axis0_Value` and a `legacy_mirrored` variant. Fixed; the default now
  targets `left_axis0_value`.
- The treadmill and the physical left thumbstick **compete for one action**.
  Skyrim's Touch binding maps `/user/hand/left/input/joystick` to
  `left_axis0_value`, and a treadmill binding has to target that same action to
  reach the game. The resting stick reports zero continuously.

Whether a `/user/treadmill` source survives that contest is still unmeasured.
Legacy state is frozen while the hand controllers are in `Standby`, so any
measurement taken with the headset off reads zero for **every** device and
proves nothing — check `GetTrackedDeviceActivityLevel` before believing a flat
axis.

## Legacy-input games cannot see the treadmill, and this is not fixable

Skyrim VR and other legacy-input titles build their controller state from the
two hand devices only. A treadmill-role device can be correctly bound, with the
binding confirmed loaded by vrserver, and still never arrive.

Measured against a live session, with `/user/treadmill/input/joystick` bound to
`/actions/legacy/in/left_axis0_value`:

```
t=14.4-16.4s   left ax0 sweeps a full unit circle   <- a physical thumbstick;
                                                       the read path is sound
t=24-40s       belt |y| up to 0.524                 <- driver log
               left ax0 = (+0.000, +0.000)          <- flat throughout
               treadmill pkt = 0                    <- never populated at all
```

The positive control matters: without a known-good input in the same trace, a
flat axis only proves that nothing happened. Here something demonstrably did.

**SteamVR does not route `/user/treadmill` sources into hand legacy state.**
Valve's own gamepad driver ships no legacy binding either, for the same reason.
Nothing in this repo can change that, which is what the next section is for.

Two traps when measuring this yourself. Controllers in `Standby` have frozen
legacy state, so a reading taken while the headset is off measures nothing —
check `GetTrackedDeviceActivityLevel` first. And a peak-magnitude heuristic
cannot tell the belt from a hand: the belt always reports `x=+0.000`, so any
sample with a non-zero `x` came from a thumbstick, not the treadmill.

## Fallback: keyboard emulation

If a game cannot see the treadmill through SteamVR at all, the companion can
send real keystrokes instead. **Companion → Keyboard fallback (legacy games).**
Off by default, and it should stay off whenever the SteamVR binding works: this
path is on/off where the real one is analog.

| Setting | Default | What it does |
| --- | --- | --- |
| Enable keyboard fallback | off | Master switch |
| Key-down threshold | 0.15 | Belt output above this holds the key |
| Hold after stop | 400 ms | Bridges the dip between footfalls, see below |
| Forward / Backward key | W / S | Held while the belt runs |
| Sprint key | Left Shift | Held while sprint is active and moving forward |
| Only send while the game is focused | **on** | Safety gate, see below |
| Game process | `SkyrimVR` | Executable name, `.exe` optional |

### Why the key has to be held after the belt stops

A belt driven by a walking human is not a steady signal. Each footfall drives
it and it coasts back between steps, so the speed crosses the threshold several
times a second even while you walk steadily. A live capture of one such walk:

```
21:59:57  y=+0.397
22:00:00  y=-0.092
22:00:04  y=+0.433
22:00:06  y=-0.229
22:00:14  y=-0.524
```

The firmware already low-pass filters this (`VELOCITY_FILTER_ALPHA`, 0.10 at
100 Hz) and it still swings, because the step cadence is slower than the
filter. That is fine for an analog stick and fatal for a key: pressing and
releasing on every crossing stutters instead of walking.

So the key presses immediately but its release is delayed by *Hold after stop*,
and releases at half the press threshold so it cannot chatter at the boundary.
Too low and you stutter; too high and you keep walking after you have stopped.

### Safety

Keystrokes are global: they go to whichever window has focus. The focus gate
exists so that walking on the belt cannot type into your browser, and it is on
by default for that reason — turn it off only if you know why you are doing
that. Keys are released when the game loses focus, when the belt stops, when
the toggle goes off, and by the destructor on exit, so quitting mid-stride
cannot leave `W` stuck down.

Windows only. On Linux the toggle reports that and does nothing.

Why keystrokes rather than a virtual gamepad: a virtual pad (ViGEm) is the
nicer answer because it is analog, but it needs a kernel driver installed with
administrator rights. Synthetic keys need nothing and every PC title reads
them.

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
| SteamVR shows treadmill input but a legacy game ignores it | Expected. Legacy titles cannot read a treadmill-role device; use the keyboard fallback. |
| Fallback works but the character stutters | Raise *Hold after stop*. The belt dips below threshold between footfalls. |
| Fallback settings revert on restart | They are only written when you press **Save** under *Config file*. |
| Second companion shows "not connected" | By design — the driver serves one companion at a time and turns away extras cleanly. Close the first one. |
| Stick never reaches full travel | Lower *Full deflection at*. |
| Companion and driver both look healthy, game does nothing | Legacy-input limitation — see the caveat above before debugging anything else. |
| Driver changes have no effect | Re-run `cmake --install` and restart SteamVR; the old `.so`/`.dll` stays loaded otherwise. |
| **Manage Bindings opens as a black empty window** | The input profile is missing `input_bindingui_left`/`input_bindingui_right`, or the image they name is not installed. See *The binding image is not decoration* above; confirm with `vrwebhelper_controllerbinding_desktop.txt`. |
| Treadmill missing from the controller list, then present after a restart | Its input profile only becomes known to SteamVR when `Activate()` runs, which can lag `TrackedDeviceAdded` by minutes while the HMD is asleep. The binding UI lists what it knew at query time. Reopen the window rather than restarting. |
| Treadmill is listed as `convr_treadmill` rather than by name | `resources/localization/localization.json` was not installed. |
| Bound correctly in SteamVR, character still does not move | Legacy-input title. The treadmill and the real left thumbstick both drive `left_axis0_value` and the resting stick reports zero. Try unbinding the left stick there; failing that, use the keyboard fallback. |
| Keyboard fallback does nothing | Focus gate: the game must be the focused window, and *Game process* must match its executable name. The status line under the toggle says which it is waiting for. |
| A key stayed down after the companion exited | Should be impossible — report it. Keys are released on stop, on focus loss, on toggle-off and in the destructor. |
| **Windows:** driver never loads, nothing in the log | 32-bit build. SteamVR only loads `bin/win64`. Reconfigure with `-A x64`. |
| **Windows:** install script refuses to run | SteamVR is still open. vrserver keeps `driver_convr.dll` locked, so installing over it would not take effect. |
| **Windows:** input freezes whenever the companion is not the focused window | An old build. `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` has to be set *before* `SDL_Init`, or SDL stops delivering joystick input on focus loss — which is every moment you are actually in the headset. |
| **Windows:** UI is tiny or blurry on a scaled display | An old build, from before the companion declared per-monitor DPI awareness and scaled the layout. |

## Layout

```
common/     IPC shim + wire format, shared by both programs
driver/     SteamVR driver (.so / .dll) + manifest and input profile
companion/  SDL2 + Dear ImGui tuning and streaming app (+ keyboard fallback)
scripts/    Windows install helper
```

Nearly all OS-specific code is inside `common/convr_ipc.cpp` — the transport is
the one thing the two platforms genuinely do differently. The remaining
`#if defined(_WIN32)` blocks are small and isolated: the config-file location
in `companion/src/config.cpp`, the DLL export macro in
`driver/src/driver_factory.cpp`, and one assertion in `common/ipc_selftest.cpp`
covering who refuses a second companion.

The companion's Windows-facing behaviour needs no `#ifdef` at all: the
DPI-awareness hint and the background-joystick hint are SDL hints that are
simply inert on Linux, and the UI scale factor comes from `SDL_GetDisplayDPI`,
which returns 1.0 on a 96 dpi display either way.

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
only the newest packet, so the driver's `RunFrame` never blocks on I/O.

It serves one companion at a time, and both platforms end up refusing a second
one cleanly — but by different routes, which is worth knowing when reading the
self-test. Windows gets it for free: the pipe is created with a single
instance, so the kernel fails the second `CreateFile` with `ERROR_PIPE_BUSY`
and the server never sees it. On Linux a second companion would otherwise sit
unaccepted in the backlog and watch its sends fail with `EAGAIN`, which looks
like a broken driver, so the server accepts and immediately closes it and
counts the rejection. Either way the newcomer gets a clean disconnect it can
report and retry from, and the incumbent is untouched.

Neither side may ever block the companion's UI thread on a wedged vrserver, so
a send that cannot complete within 100 ms drops the link and lets the normal
once-a-second reconnect pick it back up. Linux gets that from `SO_SNDTIMEO`;
Windows has no equivalent for a pipe, so the client writes overlapped and waits
on the event with a timeout.

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
