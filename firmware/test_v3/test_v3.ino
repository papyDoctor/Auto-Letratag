// Letratag V3 - board bring-up test sketch (Waveshare RP2350-Zero, arduino-pico core)
// USB serial console, 115200 baud, line ending "Newline".
// The head only heats during the h and t commands (/OE held high the rest of the time).
//
// Commands:
//   ?            help and status
//   m            measurements (VBUS, +6V5, NTC, buttons, DIAG)
//   w            measurements every 0.5 s until the next key
//   5            toggle switched +5V (EN_5V)
//   6            toggle +6V5 (EN_6V5), +5V must be on
//   e            toggle the motor driver (EN), +6V5 must be > 5.8 V
//   p <n> [v]    turn n microsteps (negative = reverse) at v microsteps/s (default 3200)
//                6400 microsteps = 1 turn = ~31.4 mm of tape; any key stops
//   s <us>       servo, pulse width in microseconds, limited to SERVO_REST_US..SERVO_CUT_US
//   s 0          servo released (no more pulses)
//   c            cut (blade to cut position then retracted, servo released)
//   h <pt>       heat a single dot (1 to 30) for HEAT_US
//   g <n>        maximum number of dots heated at the same time (default 10)
//   t            test label: 3 blocks of 40 lines (all dots, then stripes
//                of 5 dots, then the complementary stripes), fed out to the cut
//   r            ruler label: black rectangle from 0 to 6 mm, then a 0.45 mm line
//                every 5 mm from 10 to 45 mm (measured from the start of the rectangle)
//
// Buttons: A held = tape feeds (turns on +5V, +6V5 and the driver if needed,
// accelerates up to V_BUTTON, stops on release)
//          B pressed = one cut (turns on +5V if needed), like the c command

#include <Servo.h>

// Pins (RP2350 GPIOs, V3 schematic)
const uint8_t PIN_SDI    = 0;
const uint8_t PIN_CLK    = 1;
const uint8_t PIN_LE     = 2;
const uint8_t PIN_OE     = 3;   // /OE of the TLC5916s, 10k pull-up, active low
const uint8_t PIN_PB1    = 4;   // button A to GND
const uint8_t PIN_PB2    = 5;   // button B to GND
const uint8_t PIN_STEP   = 6;
const uint8_t PIN_DIR    = 7;
const uint8_t PIN_MOT_EN = 8;   // TMC2209 EN, pull-up, active low
const uint8_t PIN_DIAG   = 11;  // TMC2209 DIAG output, high = fault
const uint8_t PIN_CUTTER = 12;  // servo signal
const uint8_t PIN_EN_6V5 = 14;  // TPS61089 EN, 4k7 pull-down
const uint8_t PIN_V6V5   = 26;  // ADC0: +6V5 / (100k + 33k) x 33k
const uint8_t PIN_NTC    = 27;  // ADC1: 10k to 3V3, NTC to GND
const uint8_t PIN_VBUS   = 28;  // ADC2: VBUS / 2
const uint8_t PIN_EN_5V  = 29;  // Q2 gate, 4k7 pull-down

const float VREF_ADC   = 3.3;
const float DIV_6V5    = 133.0 / 33.0;
const float DIV_VBUS   = 2.0;
const float NTC_R25    = 10000.0;  // NTC assumed 10k, B = 3950
const float NTC_B      = 3950.0;
// Head and motor rail +6V5. It was +7V5 until 26/11/2026; R13 470k -> 390k set it to
// 6.5 V and R-EXT 270 -> 180 Ω: the TLC5916s no longer regulate the current
// (limit 105 mA, protection only), the head runs at constant voltage. A dot whose
// resistance drifts up then gets LESS power (V²/R) instead of more (I²R).
// Boost off, the rail falls to about VBUS - 0.5 V: VM_MIN tells the two apart.
const float VM_MIN     = 5.8;      // below this, the driver stays off

// Travel of the HS-5085MG cutter servo (refitted 23/11/2026). NEVER go outside this range:
// beyond it, the servo forces the mechanics.
const int SERVO_REST_US = 1050;   // blade retracted
const int SERVO_CUT_US  = 1710;   // full cut

// DIR level that moves the tape FORWARD (invert if the motor turns the wrong way)
const uint8_t DIR_FORWARD = HIGH;

const uint32_t V_START = 400;      // microsteps/s at start and stop
const float    ACCEL   = 20000.0;  // microsteps/s²

// Head
const uint8_t  NUM_DOTS                 = 30;
// Fixed heat time, same as letratag_v3 (27/11/2026). Longer pulses burn the dots
// (first head damaged after tests at 5 to 20 ms, 26/11/2026).
const uint32_t HEAT_US                  = 1500;
const uint32_t MICROSTEPS_PER_LINE_X100 = 3062;   // 12800 microsteps = 62.7 mm, line = 0.15 mm
const float    MICROSTEPS_PER_MM        = 204.0;  // 12800 microsteps = 62.7 mm
const float    HEAD_TO_BLADE_MM         = 26.0;   // dot line -> blade (measured 15/11/2026)
const float    CUT_MARGIN_MM            = 2.0;    // blank left after the last line
const uint32_t MICROSTEPS_TO_CUT        = (HEAD_TO_BLADE_MM + CUT_MARGIN_MM) * MICROSTEPS_PER_MM;
const uint32_t V_PRINT                  = 1600;   // microsteps/s while printing
const uint32_t V_BUTTON                 = 6400;   // microsteps/s, button feed (~31 mm/s)

// Bit of the 32-bit word (TLC5916 Ox output) that drives each dot.
// V3 wiring of J1: pin p = dot p-2 (pin 3 = dot 1 ... pin 32 = dot 30).
const uint8_t DOT_BIT[NUM_DOTS] = {
   0, 16,  7, 23,  1, 17,  6, 22,  5, 18,   // dots 1 to 10
   4, 19,  2, 21,  3, 20,  8, 24, 15, 25,   // dots 11 to 20
   9, 29, 14, 26, 10, 28, 13, 27, 12, 11    // dots 21 to 30
};

Servo servo;
bool on5V = false, on6V5 = false, onMot = false, servoActive = false;
uint8_t group = 10;  // dots heated at the same time (USB budget: 30 dots ~ 3.4 A on VBUS)
float v6Min, vbusMin;
String inputLine;

// True if a real key has arrived; line endings (CR, LF) left in the
// buffer after a command are discarded without counting as a stop.
bool keyReceived() {
  while (Serial.available()) {
    int ch = Serial.peek();
    if (ch != '\r' && ch != '\n') return true;
    Serial.read();
  }
  return false;
}

float readVoltage(uint8_t pin, float div) {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(pin);
  return sum / 16.0 * VREF_ADC / 4095.0 * div;
}

void motor(bool enable) {
  digitalWrite(PIN_MOT_EN, enable ? LOW : HIGH);
  onMot = enable;
}

void power6V5(bool enable) {
  if (!enable) motor(false);
  digitalWrite(PIN_EN_6V5, enable ? HIGH : LOW);
  on6V5 = enable;
}

void power5V(bool enable) {
  if (!enable) {
    power6V5(false);
    servo.detach();
    servoActive = false;
  }
  digitalWrite(PIN_EN_5V, enable ? HIGH : LOW);
  on5V = enable;
}

void measurements() {
  float vbus = readVoltage(PIN_VBUS, DIV_VBUS);
  float v6 = readVoltage(PIN_V6V5, DIV_6V5);
  float vntc = readVoltage(PIN_NTC, 1.0);
  Serial.printf("VBUS %.2f V | +6V5 %.2f V | ", vbus, v6);
  if (vntc > 3.2) {
    Serial.print("NTC not connected");
  } else {
    float r = NTC_R25 * vntc / (VREF_ADC - vntc);
    float t = 1.0 / (1.0 / 298.15 + log(r / NTC_R25) / NTC_B) - 273.15;
    Serial.printf("NTC %.0f ohm ~%.1f C", r, t);
  }
  Serial.printf(" | button A %s, B %s | DIAG %s\n",
                digitalRead(PIN_PB1) ? "released" : "PRESSED",
                digitalRead(PIN_PB2) ? "released" : "PRESSED",
                digitalRead(PIN_DIAG) ? "FAULT" : "ok");
}

void printStatus() {
  Serial.printf("+5V %s, +6V5 %s, driver %s, servo %s\n",
                on5V ? "ON" : "off", on6V5 ? "ON" : "off",
                onMot ? "ON" : "off", servoActive ? "active" : "released");
}

void printHelp() {
  Serial.println("Commands: ? m w 5 6 e  p <microsteps> [microsteps/s]  s <us> (0 = release)  c");
  Serial.println("          h <dot>  g <dots at once>  t  r");
  printStatus();
}

// Shifts 32 bits into the chain U1 -> U2 -> U3 -> U4, MSB first
// (the first bit sent ends up on U4 OUT7, bit 0 on U1 OUT0).
void loadWord(uint32_t bits) {
  for (int8_t b = 31; b >= 0; b--) {
    digitalWrite(PIN_SDI, (bits >> b) & 1);
    digitalWrite(PIN_CLK, HIGH);
    digitalWrite(PIN_CLK, LOW);
  }
  digitalWrite(PIN_LE, HIGH);
  digitalWrite(PIN_LE, LOW);
}

// Heats the requested dots (bit d-1 = dot d), at most "group" dots at a time.
// Measures +6V5 and VBUS in the middle of each pulse and keeps the minimums.
bool heat(uint32_t dots) {
  if (!on6V5) {
    Serial.println("ERR +6V5 off (command 6)");
    return false;
  }
  uint8_t d = 0;
  while (d < NUM_DOTS) {
    uint32_t bits = 0;
    uint8_t n = 0;
    for (; d < NUM_DOTS && n < group; d++) {
      if (dots & (1UL << d)) {
        bits |= 1UL << DOT_BIT[d];
        n++;
      }
    }
    if (n == 0) break;
    loadWord(bits);
    uint32_t start = micros();
    digitalWrite(PIN_OE, LOW);
    while (micros() - start < HEAT_US / 2) {}
    float v6 = analogRead(PIN_V6V5) * VREF_ADC / 4095.0 * DIV_6V5;
    float vb = analogRead(PIN_VBUS) * VREF_ADC / 4095.0 * DIV_VBUS;
    while (micros() - start < HEAT_US) {}
    digitalWrite(PIN_OE, HIGH);
    v6Min = min(v6Min, v6);
    vbusMin = min(vbusMin, vb);
  }
  loadWord(0);
  return true;
}

void cut() {
  if (!on5V) {
    Serial.println("ERR +5V off (command 5)");
    return;
  }

  if (!servoActive) servo.attach(PIN_CUTTER, SERVO_REST_US, SERVO_CUT_US);
  servo.writeMicroseconds(SERVO_CUT_US);
  delay(300);
  servo.writeMicroseconds(SERVO_REST_US);
  delay(800);
  //servo.writeMicroseconds(SERVO_REST_US);
  //delay(100);
  servo.detach();
  servoActive = false;
  Serial.println("cut done");
}

void turn(long n, uint32_t vmax, bool verbose = true);

void printTest() {
  if (!onMot) {
    Serial.println("ERR driver off (command e)");
    return;
  }
  // Stripes of 5 dots: 1-5, 11-15, 21-25, then the complement
  uint32_t allDots = (1UL << NUM_DOTS) - 1;
  uint32_t stripes = 0;
  for (uint8_t d = 0; d < NUM_DOTS; d++)
    if ((d / 5) % 2 == 0) stripes |= 1UL << d;
  const uint32_t blocks[] = {allDots, 0, stripes, 0, allDots & ~stripes};
  const uint16_t blockLines[] = {40, 10, 40, 10, 40};

  v6Min = 99;
  vbusMin = 99;
  uint32_t remainder = 0;  // hundredths of a microstep carried over from one line to the next
  uint32_t t0 = millis();
  uint16_t total = 0;
  for (uint8_t b = 0; b < 5; b++) {
    for (uint16_t l = 0; l < blockLines[b]; l++) {
      if (keyReceived()) {
        Serial.println("interrupted");
        while (Serial.available()) Serial.read();
        return;
      }
      if (blocks[b] && !heat(blocks[b])) return;
      remainder += MICROSTEPS_PER_LINE_X100;
      turn(remainder / 100, V_PRINT, false);
      remainder %= 100;
      total++;
    }
  }
  Serial.printf("%u lines in %lu ms, heat %lu us, max %u dots at once\n", total,
                (unsigned long)(millis() - t0), (unsigned long)HEAT_US, group);
  Serial.printf("during heating: +6V5 min %.2f V, VBUS min %.2f V\n", v6Min, vbusMin);
  // turn(MICROSTEPS_TO_CUT, 3200, false);
  Serial.println("label fed out to the blade (command c to cut)");
}

// Ruler label: black rectangle of 40 lines (0 to 6 mm), then a 3-line mark
// at 10, 15, 20 ... 45 mm from the start of the rectangle. If the tape slips right after the
// rectangle, all the marks land ahead of the ruler graduations.
void printRuler() {
  if (!onMot) {
    Serial.println("ERR driver off (command e)");
    return;
  }
  const uint32_t allDots = (1UL << NUM_DOTS) - 1;
  const float linePitchMm = MICROSTEPS_PER_LINE_X100 / 100.0 / MICROSTEPS_PER_MM;
  const uint16_t numLines = (uint16_t)(45.0 / linePitchMm + 0.5) + 3;  // same rounding as the marks

  v6Min = 99;
  vbusMin = 99;
  uint32_t remainder = 0;
  uint32_t t0 = millis();
  for (uint16_t l = 0; l < numLines; l++) {
    if (keyReceived()) {
      Serial.println("interrupted");
      while (Serial.available()) Serial.read();
      return;
    }
    bool black = l < 40;
    for (uint8_t mm = 10; mm <= 45; mm += 5) {
      uint16_t start = (uint16_t)(mm / linePitchMm + 0.5);
      if (l >= start && l < start + 3) black = true;
    }
    if (black && !heat(allDots)) return;
    remainder += MICROSTEPS_PER_LINE_X100;
    turn(remainder / 100, V_PRINT, false);
    remainder %= 100;
  }
  Serial.printf("%u lines in %lu ms, heat %lu us\n", numLines,
                (unsigned long)(millis() - t0), (unsigned long)HEAT_US);
  turn(MICROSTEPS_TO_CUT, 3200, false);
  Serial.println("label fed out to the blade (command c to cut)");
}

void turn(long n, uint32_t vmax, bool verbose) {
  if (!onMot) {
    Serial.println("ERR driver off (command e)");
    return;
  }
  if (n == 0) return;
  digitalWrite(PIN_DIR, n > 0 ? DIR_FORWARD : !DIR_FORWARD);
  delayMicroseconds(5);
  uint32_t total = labs(n);
  if (vmax < V_START) vmax = V_START;
  uint32_t t0 = millis();
  uint32_t i;
  for (i = 0; i < total; i++) {
    if (verbose && keyReceived()) break;
    // speed limited by the acceleration at start and at the end
    float va = sqrtf((float)V_START * V_START + 2.0f * ACCEL * i);
    float vd = sqrtf((float)V_START * V_START + 2.0f * ACCEL * (total - i));
    float v = min(min(va, vd), (float)vmax);
    uint32_t period = (uint32_t)(1e6f / v);
    uint32_t start = micros();
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_STEP, LOW);
    while (micros() - start < period) {}
  }
  if (!verbose) return;
  Serial.printf("%lu microsteps in %lu ms%s | DIAG %s\n", (unsigned long)i,
                (unsigned long)(millis() - t0), i < total ? " (interrupted)" : "",
                digitalRead(PIN_DIAG) ? "FAULT" : "ok");
  while (Serial.available()) Serial.read();
}

// Turns on +5V, +6V5 then the driver if needed. False if +6V5 does not come up.
bool prepareMotor() {
  if (onMot) return true;
  if (!on5V) { power5V(true); delay(100); }
  if (!on6V5) { power6V5(true); delay(100); }
  float v6 = readVoltage(PIN_V6V5, DIV_6V5);
  if (v6 < VM_MIN) {
    Serial.printf("ERR +6V5 too low (%.2f V)\n", v6);
    return false;
  }
  motor(true);
  printStatus();
  return true;
}

// While the button is held, the motor turns (acceleration then speed V_BUTTON);
// on release it decelerates down to V_START and stops.
void buttonFeed(uint8_t pin, bool forward) {
  delay(20);  // debounce
  if (digitalRead(pin)) return;
  if (!prepareMotor()) {
    while (!digitalRead(pin)) {}
    return;
  }
  digitalWrite(PIN_DIR, forward ? DIR_FORWARD : !DIR_FORWARD);
  delayMicroseconds(5);
  float v = V_START;
  uint32_t steps = 0, t0 = millis();
  bool held = true;
  while (held || v > V_START) {
    if (held && digitalRead(pin)) held = false;
    uint32_t period = (uint32_t)(1e6f / v);
    uint32_t start = micros();
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_STEP, LOW);
    steps++;
    // v² changes by 2.a per step
    float v2 = v * v + (held ? 2.0f : -2.0f) * ACCEL;
    v = sqrtf(max(v2, (float)V_START * V_START));
    if (v > V_BUTTON) v = V_BUTTON;
    while (micros() - start < period) {}
  }
  Serial.printf("button %s: %lu microsteps (%.1f mm) in %lu ms | DIAG %s\n", forward ? "A" : "B",
                (unsigned long)steps, steps / MICROSTEPS_PER_MM, (unsigned long)(millis() - t0),
                digitalRead(PIN_DIAG) ? "FAULT" : "ok");
}

// Button B: a single cut per press (waits for release before returning).
void buttonCut() {
  delay(20);  // debounce
  if (digitalRead(PIN_PB2)) return;
  if (!on5V) {
    power5V(true);
    delay(100);
  }
  Serial.print("button B: ");
  cut();
  while (!digitalRead(PIN_PB2)) {}
  delay(20);
}

void execute(String c) {
  c.trim();
  if (c.length() == 0) return;
  char cmd = tolower(c[0]);
  String arg = c.substring(1);
  arg.trim();

  switch (cmd) {
    case '?': printHelp(); break;
    case 'm': measurements(); break;
    case 'w':
      delay(20);
      while (Serial.available()) Serial.read();
      while (!keyReceived()) {
        measurements();
        delay(500);
      }
      while (Serial.available()) Serial.read();
      break;
    case '5':
      power5V(!on5V);
      delay(50);
      printStatus();
      measurements();
      break;
    case '6':
      if (!on6V5 && !on5V) {
        Serial.println("ERR +5V off (command 5)");
        break;
      }
      power6V5(!on6V5);
      delay(50);
      printStatus();
      measurements();
      break;
    case 'e':
      if (onMot) {
        motor(false);
      } else {
        float v6 = readVoltage(PIN_V6V5, DIV_6V5);
        if (!on6V5 || v6 < VM_MIN) {
          Serial.printf("ERR +6V5 too low (%.2f V)\n", v6);
          break;
        }
        motor(true);
      }
      printStatus();
      break;
    case 'p': {
      long n = arg.toInt();
      int sp = arg.indexOf(' ');
      uint32_t v = 3200;
      if (sp > 0) v = arg.substring(sp + 1).toInt();
      turn(n, v);
      break;
    }
    case 's': {
      long us = arg.toInt();
      if (us == 0) {
        servo.detach();
        servoActive = false;
      } else if (!on5V) {
        Serial.println("ERR +5V off (command 5)");
        break;
      } else {
        us = constrain(us, SERVO_REST_US, SERVO_CUT_US);
        if (!servoActive) {
          servo.attach(PIN_CUTTER, SERVO_REST_US, SERVO_CUT_US);
          servoActive = true;
        }
        servo.writeMicroseconds(us);
        Serial.printf("servo %ld us\n", us);
      }
      printStatus();
      break;
    }
    case 'c':
      cut();
      break;
    case 'g': {
      long n = arg.toInt();
      if (n >= 1 && n <= NUM_DOTS) group = n;
      Serial.printf("max %u dots at once\n", group);
      break;
    }
    case 'h': {
      long pt = arg.toInt();
      if (pt < 1 || pt > NUM_DOTS) {
        Serial.println("ERR dot 1 to 30");
        break;
      }
      v6Min = 99;
      vbusMin = 99;
      if (heat(1UL << (pt - 1)))
        Serial.printf("dot %ld (output O%u) heated %lu us | +6V5 %.2f V\n", pt,
                      DOT_BIT[pt - 1], (unsigned long)HEAT_US, v6Min);
      break;
    }
    case 't': {
      printTest();
      break;
    }
    case 'r': {
      printRuler();
      break;
    }
    default:
      Serial.println("ERR unknown command (? for help)");
  }
}

void setup() {
  // Everything off before anything else
  pinMode(PIN_MOT_EN, OUTPUT); digitalWrite(PIN_MOT_EN, HIGH);
  pinMode(PIN_EN_6V5, OUTPUT); digitalWrite(PIN_EN_6V5, LOW);
  pinMode(PIN_EN_5V, OUTPUT);  digitalWrite(PIN_EN_5V, LOW);
  pinMode(PIN_OE, OUTPUT);     digitalWrite(PIN_OE, HIGH);
  pinMode(PIN_SDI, OUTPUT);    digitalWrite(PIN_SDI, LOW);
  pinMode(PIN_CLK, OUTPUT);    digitalWrite(PIN_CLK, LOW);
  pinMode(PIN_LE, OUTPUT);     digitalWrite(PIN_LE, LOW);
  pinMode(PIN_STEP, OUTPUT);   digitalWrite(PIN_STEP, LOW);
  pinMode(PIN_DIR, OUTPUT);    digitalWrite(PIN_DIR, LOW);
  pinMode(PIN_PB1, INPUT_PULLUP);
  pinMode(PIN_PB2, INPUT_PULLUP);
  pinMode(PIN_DIAG, INPUT);
  analogReadResolution(12);

  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) {}
  Serial.println("\nLetratag V3 - board test");
  printHelp();
  measurements();
}

void loop() {
  if (!digitalRead(PIN_PB1)) buttonFeed(PIN_PB1, true);
  if (!digitalRead(PIN_PB2)) buttonCut();
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      execute(inputLine);
      inputLine = "";
    } else if (inputLine.length() < 40) {
      inputLine += ch;
    }
  }
}
