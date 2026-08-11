# conVR

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
