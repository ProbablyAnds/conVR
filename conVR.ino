/*
 * Quadrature encoder wiring validation - BENCH TEST ONLY
 *
 * Target : Raspberry Pi Pico 2 W (RP2350), arduino-pico core
 * Encoder: CN3806 optical quadrature, ~600 PPR, NPN open-collector A/B
 *
 * Wiring : A -> GP2, B -> GP3, encoder GND -> Pico GND (must be common!)
 *          Pull-ups: internal (INPUT_PULLUP), no external resistors assumed.
 *
 * Turn the shaft slowly by hand: the count should climb steadily one way
 * and fall the other. 600 PPR with x4 decoding = 2400 counts per revolution.
 */

const uint8_t PIN_A = 2;   // GP2 - encoder channel A
const uint8_t PIN_B = 3;   // GP3 - encoder channel B

volatile int32_t encoderCount = 0;   // signed running count, x4 decoded
volatile uint32_t errorCount  = 0;   // illegal transitions (noise / missed edges)
volatile uint8_t  lastState   = 0;   // previous (A<<1 | B), 0..3

/*
 * x4 quadrature decode, last-state method.
 *
 * State is packed as (A << 1) | B, so 0=00, 1=01, 2=10, 3=11.
 * Each edge on A or B walks the pair through a Gray-code ring where only
 * one bit changes per legal step:
 *
 *   00 -> 10 -> 11 -> 01 -> 00 ...   counted as +1 per step
 *   00 -> 01 -> 11 -> 10 -> 00 ...   counted as -1 per step
 *
 * (Which physical rotation is "+" depends on how A and B landed; if it
 * reads backwards for your setup, just swap the A and B wires.)
 *
 * We build a 4-bit index (previous state << 2) | new state and look up
 * the delta. Legal steps give +1 or -1. Two cases give 0: "no change"
 * (a spurious interrupt) and "both bits flipped" (a missed edge or a
 * glitch), which is illegal and gets tallied separately.
 */
static const int8_t QUAD_TABLE[16] = {
  //  new: 00  01  10  11
         0, -1, +1,  0,   // prev 00
        +1,  0,  0, -1,   // prev 01
        -1,  0,  0, +1,   // prev 10
         0, +1, -1,  0    // prev 11
};

// Shared ISR for both pins. Kept deliberately tiny: two reads, one table
// lookup, one add. No Serial, no delays, no floating point.
void encoderISR() {
  uint8_t state = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
  int8_t  delta = QUAD_TABLE[(lastState << 2) | state];

  if (delta != 0) {
    encoderCount += delta;
  } else if (state != lastState) {
    // Both bits changed between interrupts: a step was lost or the line
    // is bouncing/noisy. Counted so you can spot bad wiring.
    errorCount++;
  }

  lastState = state;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_A, INPUT_PULLUP);
  pinMode(PIN_B, INPUT_PULLUP);

  // Seed the state machine with where the encoder actually sits right now,
  // otherwise the very first edge looks like an illegal transition.
  lastState = (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);

  attachInterrupt(digitalPinToInterrupt(PIN_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_B), encoderISR, CHANGE);

  // Give USB CDC a moment to enumerate so the banner isn't missed.
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {
    delay(10);
  }

  Serial.println();
  Serial.println(F("=== CN3806 quadrature encoder wiring test ==="));
  Serial.println(F("Pico 2 W (RP2350) | A=GP2  B=GP3 | internal pull-ups | x4 decode"));
  Serial.println(F("600 PPR -> 2400 counts/rev. Turn the shaft slowly by hand."));
  Serial.print(F("Initial A/B state: "));
  Serial.print((lastState >> 1) & 1);
  Serial.println(lastState & 1);
  Serial.println(F("---------------------------------------------"));
}

void loop() {
  static int32_t  lastPrinted = 0;
  static uint32_t lastErrors  = 0;
  static bool     firstPrint  = true;
  static uint32_t lastPrintMs = 0;

  // ~5 Hz sampling
  if (millis() - lastPrintMs < 200) {
    return;
  }
  lastPrintMs = millis();

  // Snapshot both volatiles with interrupts off so count and error total
  // come from the same instant.
  noInterrupts();
  int32_t  count  = encoderCount;
  uint32_t errors = errorCount;
  interrupts();

  if (firstPrint || count != lastPrinted || errors != lastErrors) {
    Serial.print(F("count = "));
    Serial.print(count);

    if (!firstPrint) {
      int32_t delta = count - lastPrinted;
      Serial.print(F("  ("));
      if (delta > 0) Serial.print('+');
      Serial.print(delta);
      Serial.print(F(")"));
    }

    if (errors) {
      Serial.print(F("   [bad transitions: "));
      Serial.print(errors);
      Serial.print(F("]"));
    }

    Serial.println();

    lastPrinted = count;
    lastErrors  = errors;
    firstPrint  = false;
  }
}
