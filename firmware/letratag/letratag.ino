// Letratag LT-100H - firmware (XIAO RP2350, arduino-pico core)
// Console on the USB serial port, 115200 baud, line ending "Newline".
// Commands are blocking: feed, heat and cut never overlap.
//
// Print protocol (one command per line, upper or lower case):
//   PC -> INFO           head geometry
//   PC <- INFO first=<p> dots=<n> pitch_um=<pitch> max_lines=<max>
//                        usable dots: bits p-1 to p+n-2, pitch in micrometres
//   PC -> IMG <n> [us] [c] announces a label of n lines, optional heat time in us,
//                        c = 0 (default) motor stops at each line, 1 continuous printing
//                        (continuous is faster but less sharp)
//   PC -> L <hex>        one line of 30 dots, bit 0 = dot 1, sent n times
//   PC <- READY <n>      IMG accepted, waiting for the lines
//   PC <- P <i>/<n>      progress during printing
//   PC <- SPEED ...      continuous mode: PWM reached and number of late lines (information)
//   PC <- DONE           label fed out to the cut
//   PC <- ERR <message>  problem, the label is abandoned

#include <Servo.h>

// Pins (RP2350 GPIO numbers)
const uint8_t PIN_SDI    = 0;   // D6  -> SDI of U2
const uint8_t PIN_CLK    = 1;   // D7  -> CLK of the 4 TLC5916s
const uint8_t PIN_LE     = 2;   // D8  -> LE of the 4 TLC5916s
const uint8_t PIN_OE     = 3;   // D10 -> /OE of the 4 TLC5916s (10k pull-up, active low)
const uint8_t PIN_MOT    = 4;   // D9  -> Q2 gate
const uint8_t PIN_HALL   = 5;   // D3  <- SS460S (open collector)
const uint8_t PIN_PB1    = 26;  // D0  <- button 1 (to GND)
const uint8_t PIN_PB2    = 27;  // D1  <- button 2 (to GND)
const uint8_t PIN_CUTTER = 28;  // D2  -> MG90S servo signal

const uint8_t  NUM_DOTS     = 30;
const uint32_t DOT_MASK     = (1UL << NUM_DOTS) - 1;
const uint32_t HEAT_MAX_US  = 20000;  // safety limit on the length of one pulse
const uint32_t MOTOR_MAX_MS = 5000;
const uint32_t STALL_MS     = 500;   // with no hall edge for this long, the motor is considered stalled

// Calibration (measurements of 31/10/2026)
const uint32_t HEAT_DEFAULT_US       = 5000;
const uint8_t  PWM_FEED              = 80;    // ~53 ms per line, overshoot of 4-5 edges
const int      PWM_CONTINUOUS_MIN    = 60;    // below ~50 the motor does not start
const int      PWM_CONTINUOUS_START  = 75;
const uint32_t EDGES_PER_LINE_X100   = 1082;  // 72.1 edges/mm x 0.15 mm between dots
const uint32_t EDGES_TO_CUT          = 390;   // 5.4 mm between the dot line and the cut
const uint8_t  MAX_SIMULTANEOUS_DOTS = NUM_DOTS;  // above this, the line is heated in two halves; 16 if USB sags (1.6 A for 30 dots)
const uint8_t  PWM_FINAL             = 255;   // feed out to the cut, no precision needed
const uint8_t  FIRST_DOT             = 2;     // dot 1 (top of the label) does not touch the tape
const uint8_t  USABLE_DOTS           = NUM_DOTS - FIRST_DOT + 1;
const uint16_t PITCH_UM              = 150;   // spacing between dots, same across and along the tape

const uint16_t MAX_LINES          = 4000;   // ~600 mm of label
const uint32_t RECEIVE_TIMEOUT_MS = 30000;  // maximum delay between two received lines (enough to type them by hand)

Servo servo;
volatile uint32_t hallEdges = 0;  // cumulative counter, never reset

uint32_t image[MAX_LINES];
uint16_t numLines = 0;
uint16_t linesReceived = 0;
uint32_t heatUs = HEAT_DEFAULT_US;
bool continuous = false;
int pwmContinuous = PWM_CONTINUOUS_START;  // adjusted line after line, kept from one label to the next
bool receiving = false;
uint32_t tLastLine = 0;

void hallISR() {
  hallEdges++;
}

// Shifts 32 bits into the chain U2 -> U4 -> U6 -> U5.
// Bit (n-1) drives dot Jn; bits 30 and 31 (U5 OUT6/OUT7) are not wired.
// First bit sent = the one that ends up farthest (U5 OUT7), hence MSB first.
void loadLine(uint32_t pattern) {
  for (int8_t b = 31; b >= 0; b--) {
    digitalWrite(PIN_SDI, (pattern >> b) & 1);
    digitalWrite(PIN_CLK, HIGH);
    digitalWrite(PIN_CLK, LOW);
  }
  digitalWrite(PIN_LE, HIGH);
  digitalWrite(PIN_LE, LOW);
}

// Does not touch the motor: it may be running during heating in continuous printing
void pulse(uint32_t pattern, uint32_t us) {
  if (us > HEAT_MAX_US) us = HEAT_MAX_US;
  loadLine(pattern);
  digitalWrite(PIN_OE, LOW);
  delayMicroseconds(us);
  digitalWrite(PIN_OE, HIGH);
  loadLine(0);
}

// 30 dots x 53 mA = 1.6 A: a line with too many dots is heated in two halves
void heat(uint32_t pattern, uint32_t us) {
  if (__builtin_popcount(pattern) > MAX_SIMULTANEOUS_DOTS) {
    pulse(pattern & 0x0000FFFF, us);
    pulse(pattern & 0xFFFF0000, us);
  } else {
    pulse(pattern, us);
  }
}

void runMotor(uint32_t ms, uint8_t pwm) {
  if (ms > MOTOR_MAX_MS) ms = MOTOR_MAX_MS;
  digitalWrite(PIN_OE, HIGH);
  uint32_t start = hallEdges;
  analogWrite(PIN_MOT, pwm);
  delay(ms);
  analogWrite(PIN_MOT, 0);
  delay(200);  // let the motor stop so the last edges are counted
  Serial.printf("motor: %lu ms, pwm %u -> %lu hall edges\n", ms, pwm, hallEdges - start);
}

// Waits for the hall counter to reach the absolute target position, motor already running
bool waitForPosition(uint32_t target) {
  uint32_t last = hallEdges, tEdge = millis();
  while ((int32_t)(target - hallEdges) > 0) {
    if (hallEdges != last) {
      last = hallEdges;
      tEdge = millis();
    } else if (millis() - tEdge > STALL_MS) {
      analogWrite(PIN_MOT, 0);
      Serial.println(F("ERR motor stalled"));
      return false;
    }
  }
  return true;
}

// Runs the motor up to the absolute target position, then stops it.
// The overshoot due to inertia is thus caught up on the next step instead of accumulating.
bool feedTo(uint32_t target, uint8_t pwm) {
  digitalWrite(PIN_OE, HIGH);
  analogWrite(PIN_MOT, pwm);
  bool ok = waitForPosition(target);
  analogWrite(PIN_MOT, 0);
  return ok;
}

// Position of line i in hall edges from the start of the label (10.82 edges per line)
uint32_t linePosition(uint32_t i) {
  return (i * EDGES_PER_LINE_X100 + 50) / 100;
}

void progress(uint16_t i) {
  if ((i + 1) % 16 == 0) Serial.printf("P %u/%u\n", i + 1, numLines);
}

// Motor stopped at each line. Heating starts during the last coasting edges (4-5 at PWM 80):
// this slight slip lengthens the dots and fills the gap between lines; a fully stopped tape leaves
// white lines.
// Blank lines are neither heated nor given a stop: the motor goes straight to the next one.
bool printLineByLine(uint32_t start) {
  for (uint16_t i = 0; i < numLines; i++) {
    if (image[i]) {
      if (!feedTo(start + linePosition(i), PWM_FEED)) return false;
      heat(image[i], heatUs);
    }
    progress(i);
  }
  return true;
}

// The motor runs without stopping and each line is heated as its position goes by.
// If the previous heat overran this position, the motor slows down; if it had to wait, it speeds up.
bool printContinuous(uint32_t start) {
  uint16_t lateLines = 0;
  analogWrite(PIN_MOT, PWM_FEED);  // firm start, the adjusted PWM takes over as soon as the tape moves
  for (uint16_t i = 0; i < numLines; i++) {
    if (image[i]) {
      uint32_t target = start + linePosition(i);
      bool waited = (int32_t)(target - hallEdges) > 0;
      if (!waitForPosition(target)) return false;
      if (hallEdges != start) {
        if ((int32_t)(hallEdges - target) > 1) {
          pwmContinuous -= 2;
          lateLines++;
        } else if (waited) {
          pwmContinuous += 1;
        }
        pwmContinuous = constrain(pwmContinuous, PWM_CONTINUOUS_MIN, 255);
        analogWrite(PIN_MOT, pwmContinuous);
      }
      heat(image[i], heatUs);
    }
    progress(i);
  }
  Serial.printf("SPEED pwm=%d late=%u\n", pwmContinuous, lateLines);
  return true;
}

// Prints image[0..numLines-1] then feeds the label out to the cut
void printLabel() {
  uint32_t start = hallEdges;
  if (!(continuous ? printContinuous(start) : printLineByLine(start))) return;
  if (!feedTo(start + linePosition(numLines) + EDGES_TO_CUT, PWM_FINAL)) return;
  Serial.println(F("DONE"));
}

// 30 x 30 dot square with its diagonal: checks proportions and orientation
void printSquare() {
  for (uint8_t i = 0; i < NUM_DOTS; i++) {
    image[i] = (i == 0 || i == NUM_DOTS - 1) ? DOT_MASK
                                             : (1UL | 1UL << (NUM_DOTS - 1) | 1UL << i);
  }
  numLines = NUM_DOTS;
  heatUs = HEAT_DEFAULT_US;
  printLabel();
}

void moveServo(int angle) {
  digitalWrite(PIN_OE, HIGH);
  analogWrite(PIN_MOT, 0);
  servo.attach(PIN_CUTTER, 500, 2400);
  servo.write(constrain(angle, 0, 180));
  delay(600);
  servo.detach();
  Serial.printf("servo: %d deg\n", angle);
}

void printStatus() {
  Serial.printf("PB1=%s PB2=%s HALL=%d edges=%lu\n",
                digitalRead(PIN_PB1) ? "released" : "PRESSED",
                digitalRead(PIN_PB2) ? "released" : "PRESSED",
                digitalRead(PIN_HALL), hallEdges);
}

void printHelp() {
  Serial.println(F(
    "Printing:\n"
    "  INFO                 head geometry\n"
    "  IMG <n> [us] [c]     announce n lines (c=1: continuous), then n times: L <hex>\n"
    "  square               30 x 30 dot square with its diagonal\n"
    "  line <n> <us>        15 mm line with dot n only\n"
    "Tests:\n"
    "  status               buttons and hall sensor\n"
    "  motor <ms> [pwm]     motor for ms (pwm 0-255, default 255), counts hall edges\n"
    "  servo <deg>          cutter servo 0-180\n"
    "  dot <n> <us>         heat dot n (1-30) for us\n"
    "  pattern <hex> <us>   heat a 32-bit pattern (bit 0 = dot 1)\n"
    "  sweep <us> <ms>      heat dots 1 to 30 one by one, ms between each\n"
    "  feed <edges> [pwm]   feed by a number of hall edges\n"
    "  stairs <us> <f> [pwm] dots 1 to 30, feed f hall edges after each dot\n"
    "  measure <n>          dot n at 20 % for 5 s, for the multimeter\n"
    "  help"));
}

void execute(String line) {
  line.trim();
  if (line.length() == 0) return;

  char cmd[16] = {0};
  char arg1[16] = {0};
  long a2 = -1, a3 = -1;
  sscanf(line.c_str(), "%15s %15s %ld %ld", cmd, arg1, &a2, &a3);
  long a1 = atol(arg1);

  if (receiving) {
    if (!strcasecmp(cmd, "L") && arg1[0]) {
      image[linesReceived++] = strtoul(arg1, nullptr, 16) & DOT_MASK;
      tLastLine = millis();
      if (linesReceived == numLines) {
        receiving = false;
        printLabel();
      }
      return;
    }
    // Any other command abandons the current label, then runs normally
    receiving = false;
    Serial.printf("ERR reception interrupted at %u/%u\n", linesReceived, numLines);
  }

  if (!strcasecmp(cmd, "INFO")) {
    Serial.printf("INFO first=%u dots=%u pitch_um=%u max_lines=%u\n",
                  FIRST_DOT, USABLE_DOTS, PITCH_UM, MAX_LINES);
  } else if (!strcasecmp(cmd, "IMG")) {
    if (a1 < 1 || a1 > MAX_LINES) {
      Serial.printf("ERR IMG: 1 to %u lines\n", MAX_LINES);
      return;
    }
    numLines = a1;
    linesReceived = 0;
    heatUs = a2 > 0 ? min((uint32_t)a2, HEAT_MAX_US) : HEAT_DEFAULT_US;
    continuous = a3 == 1;
    receiving = true;
    tLastLine = millis();
    Serial.printf("READY %u\n", numLines);
  } else if (!strcasecmp(cmd, "L")) {
    Serial.println(F("ERR L without IMG"));
  } else if (!strcasecmp(cmd, "square")) {
    printSquare();
  } else if (!strcasecmp(cmd, "line") && a1 >= 1 && a1 <= NUM_DOTS && a2 > 0) {
    // 15 mm line with dot n only: long enough to judge the contact by eye
    for (uint8_t i = 0; i < 100; i++) image[i] = 1UL << (a1 - 1);
    numLines = 100;
    heatUs = min((uint32_t)a2, HEAT_MAX_US);
    printLabel();
  } else if (!strcasecmp(cmd, "status")) {
    printStatus();
  } else if (!strcasecmp(cmd, "motor") && a1 > 0) {
    runMotor(a1, a2 < 0 ? 255 : constrain(a2, 0, 255));
  } else if (!strcasecmp(cmd, "servo") && arg1[0]) {
    moveServo(a1);
  } else if (!strcasecmp(cmd, "dot") && a1 >= 1 && a1 <= NUM_DOTS && a2 > 0) {
    heat(1UL << (a1 - 1), a2);
    Serial.printf("dot %ld: %ld us\n", a1, min(a2, (long)HEAT_MAX_US));
  } else if (!strcasecmp(cmd, "pattern") && arg1[0] && a2 > 0) {
    uint32_t pattern = strtoul(arg1, nullptr, 16) & DOT_MASK;
    heat(pattern, a2);
    Serial.printf("pattern %08lX: %ld us\n", pattern, min(a2, (long)HEAT_MAX_US));
  } else if (!strcasecmp(cmd, "measure") && a1 >= 1 && a1 <= NUM_DOTS) {
    // 20 % duty cycle for 5 s: readable on a multimeter, safe for the dot
    Serial.printf("measure dot %ld: 5 s at 20 %%, measure the voltage on pad J%ld\n", a1, a1);
    analogWrite(PIN_MOT, 0);
    loadLine(1UL << (a1 - 1));
    for (uint32_t t0 = millis(); millis() - t0 < 5000;) {
      digitalWrite(PIN_OE, LOW);
      delayMicroseconds(1000);
      digitalWrite(PIN_OE, HIGH);
      delayMicroseconds(4000);
    }
    loadLine(0);
    Serial.println(F("measurement done"));
  } else if (!strcasecmp(cmd, "feed") && a1 > 0) {
    uint32_t start = hallEdges;
    if (feedTo(start + a1, a2 < 0 ? 255 : constrain(a2, 0, 255))) {
      delay(200);
      Serial.printf("feed: %ld edges requested, %lu obtained\n", a1, hallEdges - start);
    }
  } else if (!strcasecmp(cmd, "stairs") && a1 > 0 && a2 > 0) {
    // One dot per line, feed a2 edges between each: draws a diagonal on the tape
    uint32_t start = hallEdges, target = start, t0 = millis();
    uint8_t n;
    for (n = 1; n <= NUM_DOTS; n++) {
      heat(1UL << (n - 1), a1);
      target += a2;
      if (!feedTo(target, a3 < 0 ? 255 : constrain(a3, 0, 255))) break;
    }
    uint32_t duration = millis() - t0;
    delay(200);
    Serial.printf("stairs: %u lines, %lu edges (target %lu) in %lu ms\n",
                  n - 1, hallEdges - start, target - start, duration);
  } else if (!strcasecmp(cmd, "sweep") && a1 > 0 && a2 >= 0) {
    for (uint8_t n = 1; n <= NUM_DOTS; n++) {
      heat(1UL << (n - 1), a1);
      Serial.printf("dot %u\n", n);
      delay(a2);
    }
  } else {
    printHelp();
  }
}

void setup() {
  // Head off and motor stopped before anything else
  pinMode(PIN_OE, OUTPUT);
  digitalWrite(PIN_OE, HIGH);
  pinMode(PIN_MOT, OUTPUT);
  digitalWrite(PIN_MOT, LOW);

  pinMode(PIN_SDI, OUTPUT);
  pinMode(PIN_CLK, OUTPUT);
  pinMode(PIN_LE, OUTPUT);
  digitalWrite(PIN_CLK, LOW);
  digitalWrite(PIN_LE, LOW);
  loadLine(0);

  analogWriteFreq(20000);
  analogWriteRange(255);
  analogWrite(PIN_MOT, 0);

  pinMode(PIN_HALL, INPUT_PULLUP);
  pinMode(PIN_PB1, INPUT_PULLUP);
  pinMode(PIN_PB2, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_HALL), hallISR, CHANGE);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println(F("Letratag"));
  printHelp();
}

void loop() {
  static String buffer;
  static uint8_t pb1 = HIGH, pb2 = HIGH;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      execute(buffer);
      buffer = "";
    } else if (buffer.length() < 64) {
      buffer += c;
    }
  }

  if (receiving && millis() - tLastLine > RECEIVE_TIMEOUT_MS) {
    receiving = false;
    Serial.printf("ERR timeout at %u/%u lines\n", linesReceived, numLines);
  }

  // Reports the buttons; the hall sensor is no longer shown so as not to clutter the protocol
  uint8_t p1 = digitalRead(PIN_PB1), p2 = digitalRead(PIN_PB2);
  if (p1 != pb1 || p2 != pb2) {
    pb1 = p1; pb2 = p2;
    printStatus();
    delay(20);  // crude debounce
  }
}
