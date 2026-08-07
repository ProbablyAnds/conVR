# conVR

USB game-controller firmware for a VR treadmill belt encoder, built around a
Raspberry Pi Pico 2 W and a CN3806 optical quadrature encoder.

## Hardware

- Raspberry Pi Pico 2 W
- CN3806 open-collector quadrature encoder, 600 PPR
- 70 mm friction disk
- Two 1 kOhm pull-up resistors

| Encoder wire | Pico connection |
| --- | --- |
| Red | VBUS (5 V) |
| Black | GND |
| Green / phase A | GP2, plus 1 kOhm to 3V3 |
| White / phase B | GP3, plus 1 kOhm to 3V3 |

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
- 6 m/s: full stick travel by default

Edit the constants near the top of `conVR.ino` to change maximum speed,
direction, filtering, or the stop timeout. Set `ENCODER_DIRECTION` to `-1` if
forward movement is reported as a negative speed in the serial monitor.

Serial diagnostics run at 115200 baud and show position, count delta, raw and
filtered belt speed, and the resulting joystick value.

## Implementation

The official Raspberry Pi quadrature PIO program counts every A/B transition
without CPU interrupts. Firmware samples its signed 32-bit count at 100 Hz,
converts the change to metres per second, applies a low-latency filter, and
sends a USB HID report. A 40 ms no-motion timeout hard-centres the controller.
