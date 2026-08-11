/*
 * conVR treadmill encoder -> USB joystick firmware
 *
 * Target : Raspberry Pi Pico 2 W (RP2350), arduino-pico core
 * Encoder: CN3806, 600 PPR, NPN open-collector quadrature output
 *
 * Wiring:
 *   Encoder red   -> VBUS (5 V)
 *   Encoder black -> GND
 *   Encoder green -> GP2 (phase A)
 *   Encoder white -> GP3 (phase B)
 *   1 kOhm from GP2 to 3V3 and 1 kOhm from GP3 to 3V3
 *   Pushbutton between GP4 and GND (physical pin 38)
 *
 * The encoder count is maintained entirely by a PIO state machine. The CPU
 * samples that count at 100 Hz, converts the delta into belt velocity, and
 * reports velocity as the USB gamepad's left-stick Y axis.
 */

#include <Arduino.h>
#include <Joystick.h>
#include <USB.h>
#include "hardware/pio.h"
#include "quadrature_encoder.pio.h"

// Ask the USB host to poll HID reports every millisecond. New velocity reports
// are produced every 10 ms; the shorter USB interval reduces delivery latency.
int usb_hid_poll_interval = 1;

namespace Config {

constexpr uint8_t PIN_A = 2;
constexpr uint8_t PIN_B = 3;  // The PIO program requires B to immediately follow A.
constexpr uint8_t PIN_BUTTON = 4;
constexpr uint8_t SPRINT_BUTTON = 9;  // Conventional gamepad left-stick click.

constexpr float ENCODER_PULSES_PER_REVOLUTION = 600.0f;
constexpr float QUADRATURE_COUNTS_PER_PULSE = 4.0f;
constexpr float FRICTION_DISK_DIAMETER_METRES = 0.075f;
constexpr float COUNTS_PER_METRE =
    (ENCODER_PULSES_PER_REVOLUTION * QUADRATURE_COUNTS_PER_PULSE) /
    (PI * FRICTION_DISK_DIAMETER_METRES);

// This belt speed maps to full joystick deflection. Raise it if the axis clips
// during a sprint, or lower it if games do not receive enough stick travel.
constexpr float MAX_BELT_SPEED_MPS = 1.4f;  // 5.04 km/h, calibrated on treadmill
constexpr float SPRINT_START_SPEED_MPS = 0.80f;
constexpr float SPRINT_RELEASE_SPEED_MPS = 0.65f;

// Set to -1 if the serial output reports negative speed while walking forward.
constexpr int8_t ENCODER_DIRECTION = 1;

constexpr uint32_t SAMPLE_INTERVAL_US = 10000;  // 100 Hz speed calculation
constexpr uint32_t STOP_TIMEOUT_US = 120000;    // hard-centre after 120 ms idle
constexpr uint32_t DIAGNOSTIC_INTERVAL_MS = 250;

// Exponential smoothing. 1.0 is unfiltered; smaller values are smoother but
// add latency. At 100 Hz, 0.10 smooths the starts and stops within each step;
// the separate stop timeout still centres immediately when motion has ended.
constexpr float VELOCITY_FILTER_ALPHA = 0.10f;
constexpr float CENTRE_DEADBAND_MPS = 0.02f;

}  // namespace Config

static_assert(Config::PIN_B == Config::PIN_A + 1,
              "The quadrature PIO program requires consecutive A/B pins");

PIO encoderPio = pio0;
int encoderStateMachine = -1;

int32_t previousCount = 0;
uint32_t previousSampleUs = 0;
uint32_t lastMotionUs = 0;
float filteredSpeedMps = 0.0f;
bool sprintActive = false;
bool serialWasConnected = false;

int16_t speedToJoystickAxis(float speedMps) {
  float normalised = speedMps / Config::MAX_BELT_SPEED_MPS;
  normalised = constrain(normalised, -1.0f, 1.0f);

  // Negative Y is "stick forward" in the conventional gamepad coordinate
  // system, so positive treadmill speed is deliberately inverted here.
  return static_cast<int16_t>(lroundf(-normalised * 32767.0f));
}

void centreJoystick() {
  Joystick.position(0, 0);
  Joystick.send_now();
}

void haltWithError(const __FlashStringHelper *message) {
  centreJoystick();
  while (true) {
    Serial.println(message);
    delay(1000);
  }
}

void startEncoderPio() {
  if (!pio_can_add_program(encoderPio, &quadrature_encoder_program)) {
    haltWithError(F("ERROR: PIO0 instruction memory is unavailable"));
  }

  encoderStateMachine = pio_claim_unused_sm(encoderPio, false);
  if (encoderStateMachine < 0) {
    haltWithError(F("ERROR: PIO0 has no unused state machine"));
  }

  // This official Raspberry Pi program has a fixed origin of zero because it
  // uses computed jumps. No other PIO programs are used by this sketch.
  pio_add_program(encoderPio, &quadrature_encoder_program);
  quadrature_encoder_program_init(
      encoderPio, static_cast<uint>(encoderStateMachine), Config::PIN_A, 0);
}

void printStartupBanner() {
  Serial.println();
  Serial.println(F("=== conVR treadmill controller ==="));
  Serial.println(F("PIO quadrature counter: GP2=A, GP3=B, x4 decoding"));
  Serial.println(F("USB HID: 16-bit left-stick Y axis, centre=0"));
  Serial.print(F("Counts per metre: "));
  Serial.println(Config::COUNTS_PER_METRE, 2);
  Serial.print(F("Full stick speed: "));
  Serial.print(Config::MAX_BELT_SPEED_MPS, 2);
  Serial.println(F(" m/s"));
  Serial.println(
      F("count\tdelta\traw_m/s\tfiltered_m/s\taxis\tbutton\tsprint"));
}

void updateSerialConnection() {
  const bool serialConnected = static_cast<bool>(Serial);
  if (serialConnected && !serialWasConnected) {
    printStartupBanner();
  }
  serialWasConnected = serialConnected;
}

void setup() {
  Serial.begin(115200);
  pinMode(Config::PIN_BUTTON, INPUT_PULLUP);

  // The arduino-pico Joystick library works with Tools > USB Stack > Pico SDK.
  // Manual mode lets both axes be changed before emitting one coherent report.
  USB.disconnect();
  USB.setProduct("conVR Treadmill Controller");
  Joystick.use16bit();
  Joystick.useManualSend(true);
  Joystick.begin();
  centreJoystick();

  startEncoderPio();
  previousCount = quadrature_encoder_get_count(
      encoderPio, static_cast<uint>(encoderStateMachine));
  previousSampleUs = micros();
  lastMotionUs = previousSampleUs;

  // Serial is diagnostic only. Controller operation never waits for a monitor.
  updateSerialConnection();
}

void loop() {
  updateSerialConnection();

  const uint32_t nowUs = micros();
  const uint32_t elapsedUs = nowUs - previousSampleUs;
  if (elapsedUs < Config::SAMPLE_INTERVAL_US) {
    return;
  }

  const int32_t count = quadrature_encoder_get_count(
      encoderPio, static_cast<uint>(encoderStateMachine));

  // Unsigned subtraction makes the delta well-defined even when the PIO's
  // signed 32-bit position counter eventually wraps around.
  const int32_t delta = static_cast<int32_t>(
      static_cast<uint32_t>(count) - static_cast<uint32_t>(previousCount));

  previousCount = count;
  previousSampleUs = nowUs;

  float rawSpeedMps =
      (static_cast<float>(delta) * Config::ENCODER_DIRECTION * 1000000.0f) /
      (Config::COUNTS_PER_METRE * static_cast<float>(elapsedUs));

  if (delta != 0) {
    lastMotionUs = nowUs;
  }

  // Filtering makes normal movement feel steady. The explicit stop timeout is
  // intentionally outside the filter so releasing the belt cannot leave a
  // slowly decaying joystick command.
  filteredSpeedMps += Config::VELOCITY_FILTER_ALPHA *
                      (rawSpeedMps - filteredSpeedMps);

  if (nowUs - lastMotionUs >= Config::STOP_TIMEOUT_US) {
    rawSpeedMps = 0.0f;
    filteredSpeedMps = 0.0f;
  }

  int16_t axis = speedToJoystickAxis(filteredSpeedMps);
  if (fabsf(filteredSpeedMps) < Config::CENTRE_DEADBAND_MPS) {
    axis = 0;
  }

  if (filteredSpeedMps >= Config::SPRINT_START_SPEED_MPS) {
    sprintActive = true;
  } else if (filteredSpeedMps <= Config::SPRINT_RELEASE_SPEED_MPS) {
    sprintActive = false;
  }

  const bool buttonPressed = digitalRead(Config::PIN_BUTTON) == LOW;
  Joystick.position(0, axis);
  Joystick.button(1, buttonPressed);
  Joystick.button(Config::SPRINT_BUTTON, sprintActive);
  Joystick.send_now();

  static uint32_t lastDiagnosticMs = 0;
  static bool previousDiagnosticButton = false;
  static bool previousDiagnosticSprint = false;
  const uint32_t nowMs = millis();
  const bool inputChanged = buttonPressed != previousDiagnosticButton ||
                            sprintActive != previousDiagnosticSprint;
  if (Serial && (inputChanged ||
                 nowMs - lastDiagnosticMs >= Config::DIAGNOSTIC_INTERVAL_MS)) {
    lastDiagnosticMs = nowMs;
    previousDiagnosticButton = buttonPressed;
    previousDiagnosticSprint = sprintActive;
    Serial.print(count);
    Serial.print('\t');
    Serial.print(delta);
    Serial.print('\t');
    Serial.print(rawSpeedMps, 3);
    Serial.print('\t');
    Serial.print(filteredSpeedMps, 3);
    Serial.print('\t');
    Serial.print(axis);
    Serial.print('\t');
    Serial.print(buttonPressed ? 1 : 0);
    Serial.print('\t');
    Serial.println(sprintActive ? 1 : 0);
  }
}
