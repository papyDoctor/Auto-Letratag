// Letratag LT-100H - firmware for the V3 board (Waveshare RP2350-Zero, arduino-pico core)
// Console on the USB serial port, 115200 baud, line ending "Newline".
// Commands are blocking. In continuous printing, feeding and heating overlap;
// cutting, however, never happens during heating or feeding.
//
// Chained labels: each label is [margin][text][margin]. The firmware remembers the tape
// position of each boundary between labels and cuts when that boundary reaches the blade
// (head -> blade distance is adjustable, blade command), even in the middle of printing
// the next label. No tape is wasted between two labels.
//
// Print protocol (one command per line, upper or lower case), compatible with the
// rev B one (firmware/letratag):
//   PC -> INFO              head geometry
//   PC <- INFO first=<p> dots=<n> pitch_um=<pitch> max_lines=<max> cut=2
//                           usable dots: bits p-1 to p+n-2, pitch in micrometres;
//                           cut=2: automatic cut and chained labels
//   PC -> IMG <n> [us] [c] [k]
//                           announces a label of n lines; us = heat time (default 5000);
//                           c = 1 (default) continuous, 0 motor stops at each line;
//                           k = 1 (default) the label is fed out and cut,
//                           k = 0 another one follows: its cut happens during the next one
//   PC -> L <hex>           one line of 30 dots, bit 0 = dot 1 (top), sent n times
//   PC -> FLUSH             feeds out and cuts everything printed (end of batch), then DONE
//   PC <- READY <n>         IMG accepted, waiting for the lines
//   PC <- P <i>/<n>         progress during printing
//   PC <- SPEED ...         continuous mode: speed and late lines (information)
//   PC <- VOLTAGES ...      lowest +7V5 and VBUS during heating (information)
//   PC <- DONE              label printed (and fed out and cut if k = 1)
//   PC <- ERR <message>     problem, the label is abandoned
//
// Buttons: A held = tape feeds; B = feeds out and cuts what is printed (or a plain cut
// if nothing is pending). A button pressed during printing stops it.
// Power: +5V, +7V5 and driver turn on on demand and turn off after AUTO_OFF_MS
// without activity.

#include <Servo.h>
#include <EEPROM.h>

// ---------------------------------------------------------------- pins (V3 schematic)
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
const uint8_t PIN_EN_7V5 = 14;  // TPS61089 EN, 4k7 pull-down
const uint8_t PIN_V7V5   = 26;  // ADC0: +7V5 / (100k + 33k) x 33k
const uint8_t PIN_NTC    = 27;  // ADC1: 10k to 3V3, NTC to GND (not fitted yet)
const uint8_t PIN_VBUS   = 28;  // ADC2: VBUS / 2
const uint8_t PIN_EN_5V  = 29;  // Q2 gate, 4k7 pull-down

// ---------------------------------------------------------------- analog measurements
const float VREF_ADC = 3.3;
const float DIV_7V5  = 133.0 / 33.0;
const float DIV_VBUS = 2.0;
const float VM_MIN   = 6.5;   // below this, the driver stays off
const float VBUS_MIN = 4.4;   // below this, printing is refused (USB too weak)

// ---------------------------------------------------------------- cutter
// HS-5085MG servo (refitted 23/11/2026), travel set by the user. NEVER go outside this range:
// beyond it, the servo forces the mechanics.
const int SERVO_REST_US = 1050;   // blade retracted
const int SERVO_CUT_US  = 1710;   // full cut

// ---------------------------------------------------------------- motor and feed
const uint8_t  DIR_FORWARD = HIGH;    // DIR level that feeds the tape forward
const uint32_t V_START     = 400;     // usteps/s at start and stop
const float    ACCEL       = 20000.0; // usteps/s²
const uint32_t V_LINE      = 1600;    // usteps/s, one-line feed in stop mode
const uint32_t V_FAST      = 3200;    // usteps/s, blank lines and feed out to the blade
const uint32_t V_BUTTON    = 6400;    // usteps/s, feed with button A (~31 mm/s)
const uint32_t V_CONTINUOUS_MAX = 6400;  // usteps/s, ceiling in continuous printing
const float    USTEPS_PER_MM = 204.0;    // 12800 usteps = 62.7 mm (15/11/2026)
const uint32_t USTEPS_PER_LINE_X100 = 3062;  // 0.15 mm per line

// ---------------------------------------------------------------- head
const uint8_t  NUM_DOTS        = 30;
const uint32_t DOT_MASK        = (1UL << NUM_DOTS) - 1;
const uint32_t HEAT_MAX_US     = 20000;  // safety limit on one pulse
const uint32_t HEAT_DEFAULT_US = 5000;   // solid black with the head well seated (22/11/2026)
const uint8_t  FIRST_DOT       = 1;      // V3: dot 1 prints too (line 1, 22/11/2026)
const uint8_t  USABLE_DOTS     = NUM_DOTS - FIRST_DOT + 1;
const uint16_t PITCH_UM        = 150;    // dot spacing, same across and along the tape

// Bit of the 32-bit word (TLC5916 Ox outputs) that drives each dot.
// V3 wiring of J1: pin p = dot p-2 (confirmed by the test label on 15/11/2026).
const uint8_t DOT_BIT[NUM_DOTS] = {
   0, 16,  7, 23,  1, 17,  6, 22,  5, 18,   // dots 1 to 10
   4, 19,  2, 21,  3, 20,  8, 24, 15, 25,   // dots 11 to 20
   9, 29, 14, 26, 10, 28, 13, 27, 12, 11    // dots 21 to 30
};

// Protocol dot numbers (bit 0 = dot 1) are counted from the TOP of the label, as on
// rev B. On V3 the head is wired the other way round: without this flip, the text came
// out upside down compared to the Dymo (22/11/2026).
const bool HEAD_FLIPPED = true;

const uint16_t MAX_LINES          = 4000;   // ~600 mm of label
const uint32_t RECEIVE_TIMEOUT_MS = 30000;  // maximum delay between two received lines
const uint32_t AUTO_OFF_MS        = 60000;  // power off after 1 min without activity

// ---------------------------------------------------------------- settings in flash
// Changeable by command without recompiling (the cutter plate is going to change).
struct Settings {
  uint32_t magic;
  float headToBladeMm;  // dot row -> blade
  float marginMm;       // blank before and after the text of each label
  uint8_t group;        // dots heated at the same time (30 dots ~ 3.4 A on VBUS)
};
const uint32_t MAGIC = 0x4C543302;
const Settings DEFAULT_SETTINGS = {MAGIC, 57.5, 2.0, 10};  // blade 57.5 mm: calib of 22/11/2026
Settings settings = DEFAULT_SETTINGS;

// ---------------------------------------------------------------- state
Servo servo;
bool on5V = false, on7V5 = false, onMot = false;
uint32_t image[MAX_LINES];
uint16_t numLines = 0, linesReceived = 0;
uint32_t heatUs = HEAT_DEFAULT_US;
bool continuous = true, receiving = false;
int flushAfter = 1;   // k of IMG: 1 = feed out and cut, 0 = chain
uint32_t tLastLine = 0, tActivity = 0;
float v7Min, vbusMin;

// Tape position: usteps fed since startup. A point on the tape is identified by the
// value of tapePos when it passed under the dot row; it reaches the blade when tapePos
// equals that point + head -> blade distance.
uint32_t tapePos = 0;
const uint8_t MAX_CUTS = 16;
uint32_t cuts[MAX_CUTS];   // tape points to cut, in order
uint8_t numCuts = 0;
uint32_t lastCut = 0;
bool hasCut = false;

// Continuous heating, driven step by step by feed()
struct {
  bool active;         // a label is being printed continuously
  uint32_t start;      // tapePos of line 0
  uint16_t i;          // next line to heat
  bool heating;        // a group is currently heating
  uint32_t tOE, us, dots;
  uint8_t d;           // next group of the current line
  uint16_t late;
} job;

// ================================================================ settings

int32_t bladeUsteps() { return lroundf(settings.headToBladeMm * USTEPS_PER_MM); }
int32_t marginUsteps() { return lroundf(settings.marginMm * USTEPS_PER_MM); }

void loadSettings() {
  EEPROM.begin(256);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == MAGIC && s.headToBladeMm >= 0 && s.headToBladeMm <= 200 && s.marginMm >= 0 &&
      s.marginMm <= 50 && s.group >= 1 && s.group <= NUM_DOTS)
    settings = s;
}

void saveSettings() {
  EEPROM.put(0, settings);
  EEPROM.commit();
}

// ================================================================ power

float readVoltage(uint8_t pin, float div) {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(pin);
  return sum / 16.0 * VREF_ADC / 4095.0 * div;
}

void motor(bool active) {
  digitalWrite(PIN_MOT_EN, active ? LOW : HIGH);
  onMot = active;
}

// Turns everything off: driver, then +7V5 (VM before VIO is never reversed), then +5V
void powerOffAll() {
  digitalWrite(PIN_OE, HIGH);
  motor(false);
  digitalWrite(PIN_EN_7V5, LOW);
  on7V5 = false;
  servo.detach();
  digitalWrite(PIN_EN_5V, LOW);
  on5V = false;
}

void powerOn5V() {
  if (!on5V) {
    digitalWrite(PIN_EN_5V, HIGH);
    on5V = true;
    delay(100);
  }
  tActivity = millis();
}

// +5V, +7V5 then driver. False (and ERR message) if a voltage is missing.
bool powerOnAll() {
  tActivity = millis();
  if (onMot) return true;
  powerOn5V();
  float vbus = readVoltage(PIN_VBUS, DIV_VBUS);
  if (vbus < VBUS_MIN) {
    Serial.printf("ERR VBUS too low (%.2f V)\n", vbus);
    return false;
  }
  if (!on7V5) {
    digitalWrite(PIN_EN_7V5, HIGH);
    on7V5 = true;
    delay(100);
  }
  float v7 = readVoltage(PIN_V7V5, DIV_7V5);
  if (v7 < VM_MIN) {
    Serial.printf("ERR +7V5 too low (%.2f V)\n", v7);
    return false;
  }
  motor(true);
  delay(5);
  return true;
}

// ================================================================ head

// 32 bits into the U1 -> U2 -> U3 -> U4 chain, MSB first (bit 0 = U1 OUT0)
void loadBits(uint32_t bits) {
  for (int8_t b = 31; b >= 0; b--) {
    digitalWrite(PIN_SDI, (bits >> b) & 1);
    digitalWrite(PIN_CLK, HIGH);
    digitalWrite(PIN_CLK, LOW);
  }
  digitalWrite(PIN_LE, HIGH);
  digitalWrite(PIN_LE, LOW);
}

uint8_t numGroups(uint32_t dots) {
  uint8_t n = __builtin_popcount(dots & DOT_MASK);
  return (n + settings.group - 1) / settings.group;
}

// Group g of a line (at most settings.group dots), then g moves to the next one.
// Returns the word for the TLC5916s, 0 when all groups are done.
// Groups are interleaved (1 dot in ng) instead of being slices 1-10, 11-20, 21-30:
// in continuous mode the tape moves between two groups, and slices gave slanted text
// (the bottom of the line heated ~0.1 mm further than the top, 22/11/2026).
uint32_t groupBits(uint32_t dots, uint8_t &g) {
  uint8_t ng = numGroups(dots);
  if (g >= ng) return 0;
  uint32_t bits = 0;
  uint8_t rank = 0;
  for (uint8_t d = 0; d < NUM_DOTS; d++) {
    if (dots & (1UL << d)) {
      if (rank % ng == g) bits |= 1UL << DOT_BIT[HEAD_FLIPPED ? NUM_DOTS - 1 - d : d];
      rank++;
    }
  }
  g++;
  return bits;
}

void recordVoltages() {
  v7Min = min(v7Min, analogRead(PIN_V7V5) * VREF_ADC / 4095.0f * DIV_7V5);
  vbusMin = min(vbusMin, analogRead(PIN_VBUS) * VREF_ADC / 4095.0f * DIV_VBUS);
}

// Heats one line, motor stopped, group after group; measures the voltages halfway
void heatLine(uint32_t dots, uint32_t us) {
  us = min(us, HEAT_MAX_US);
  uint8_t d = 0;
  while (uint32_t bits = groupBits(dots, d)) {
    loadBits(bits);
    uint32_t start = micros();
    digitalWrite(PIN_OE, LOW);
    while (micros() - start < us / 2) {}
    recordVoltages();
    while (micros() - start < us) {}
    digitalWrite(PIN_OE, HIGH);
  }
  loadBits(0);
}

void reportProgress(uint16_t i) {
  if ((i + 1) % 16 == 0) Serial.printf("P %u/%u\n", i + 1, numLines);
}

// Position of line i in usteps from line 0
uint32_t linePosition(uint32_t i) {
  return (i * USTEPS_PER_LINE_X100 + 50) / 100;
}

// Continuous heating: called constantly by feed(). Starts heating a line when the tape
// reaches its position, then runs through its groups.
void serviceHeat() {
  if (!job.active) return;
  if (job.heating) {
    if (micros() - job.tOE < job.us) return;
    recordVoltages();
    digitalWrite(PIN_OE, HIGH);
    uint32_t bits = groupBits(job.dots, job.d);
    if (bits) {
      loadBits(bits);
      job.tOE = micros();
      digitalWrite(PIN_OE, LOW);
    } else {
      job.heating = false;
    }
    return;
  }
  if (job.i >= numLines) return;
  uint32_t target = job.start + linePosition(job.i);
  if ((int32_t)(tapePos - target) < 0) return;
  if ((int32_t)(tapePos - target) > 2) job.late++;
  job.dots = image[job.i];
  job.d = 0;
  uint32_t bits = groupBits(job.dots, job.d);
  if (bits) {
    loadBits(bits);
    job.tOE = micros();
    digitalWrite(PIN_OE, LOW);
    job.heating = true;
  }
  reportProgress(job.i);
  job.i++;
}

// True if the next line is reached while the previous one is still heating
bool heatRunningLate() {
  return job.active && job.heating && job.i < numLines &&
         (int32_t)(tapePos - (job.start + linePosition(job.i))) >= 0;
}

// ================================================================ cutter

void cut() {
  digitalWrite(PIN_OE, HIGH);
  powerOn5V();
  servo.attach(PIN_CUTTER, SERVO_REST_US, SERVO_CUT_US);
  servo.writeMicroseconds(SERVO_REST_US);
  delay(300);
  servo.writeMicroseconds(SERVO_CUT_US);
  delay(500);
  servo.writeMicroseconds(SERVO_REST_US);
  delay(500);
  servo.detach();
  tActivity = millis();
}

// Adds a cut at tape point p, unless a cut is already scheduled (or done) less than
// 1 mm away: two chained labels share the same boundary.
void scheduleCut(uint32_t p) {
  const int32_t near = USTEPS_PER_MM;
  if (numCuts && labs((int32_t)(p - cuts[numCuts - 1])) < near) return;
  if (!numCuts && hasCut && labs((int32_t)(p - lastCut)) < near) return;
  if (numCuts == MAX_CUTS) {
    Serial.println("ERR too many pending cuts (use flush)");
    return;
  }
  cuts[numCuts++] = p;
}

// Usteps left before the next cut reaches the blade (very large if there is none)
int32_t stepsToCut() {
  if (!numCuts) return INT32_MAX;
  return (int32_t)(cuts[0] + bladeUsteps() - tapePos);
}

// Scheduled cut reached: motor stopped, finish the current heating, then cut
void doCut() {
  while (job.active && job.heating) serviceHeat();
  digitalWrite(PIN_OE, HIGH);
  cut();
  lastCut = cuts[0];
  hasCut = true;
  numCuts--;
  for (uint8_t j = 0; j < numCuts; j++) cuts[j] = cuts[j + 1];
}

// ================================================================ motor

inline void oneStep() {
  digitalWrite(PIN_STEP, HIGH);
  delayMicroseconds(2);
  digitalWrite(PIN_STEP, LOW);
}

bool buttonPressed() {
  return !digitalRead(PIN_PB1) || !digitalRead(PIN_PB2);
}

// Stop requested by a button
bool stopRequested() {
  if (!buttonPressed()) return false;
  digitalWrite(PIN_OE, HIGH);
  return true;
}

// Feeds n usteps, at most at vmax, with acceleration and braking ramps.
// The motor brakes to stop exactly on each scheduled cut, cuts, then restarts.
// Also drives continuous heating (job); if a line has not finished heating when the
// next one arrives, the motor waits for it. Returns the number of usteps done.
uint32_t feed(uint32_t n, float vmax, bool (*stop)() = nullptr) {
  if (!onMot) return 0;
  digitalWrite(PIN_DIR, DIR_FORWARD);
  delayMicroseconds(5);
  if (vmax < V_START) vmax = V_START;
  const float v02 = (float)V_START * V_START;
  uint32_t i = 0, k = 0;         // k: steps since the last start
  uint32_t tStep = 0, period = 0;
  bool stopped = true;
  while (true) {
    serviceHeat();
    if (stepsToCut() <= 0) {
      doCut();
      digitalWrite(PIN_DIR, DIR_FORWARD);
      k = 0;
      stopped = true;
      continue;
    }
    if (i >= n) break;
    if (heatRunningLate()) {
      // wait for the heating to finish; restart gently afterwards
      if (!stopped && micros() - tStep >= period) {
        k = 0;
        stopped = true;
      }
      continue;
    }
    uint32_t t = micros();
    if (!stopped && t - tStep < period) continue;
    if (stop && stop()) break;
    uint32_t remaining = n - i;
    int32_t sc = stepsToCut();
    if (sc < (int32_t)remaining) remaining = sc;
    float v = min(vmax, sqrtf(v02 + 2.0f * ACCEL * k));
    v = min(v, sqrtf(v02 + 2.0f * ACCEL * (remaining - 1)));
    oneStep();
    tapePos++;
    i++;
    k++;
    tStep = t;
    period = (uint32_t)(1e6f / v);
    stopped = false;
  }
  // let the last period elapse before returning
  while (!stopped && micros() - tStep < period) serviceHeat();
  tActivity = millis();
  return i;
}

// Reverses n usteps (no cut)
void reverse(uint32_t n) {
  if (!onMot || !n) return;
  digitalWrite(PIN_DIR, !DIR_FORWARD);
  delayMicroseconds(5);
  for (uint32_t i = 0; i < n; i++) {
    float va = sqrtf((float)V_START * V_START + 2.0f * ACCEL * i);
    float vd = sqrtf((float)V_START * V_START + 2.0f * ACCEL * (n - i));
    float v = min(min(va, vd), (float)V_FAST);
    uint32_t start = micros();
    oneStep();
    tapePos--;
    uint32_t p = (uint32_t)(1e6f / v);
    while (micros() - start < p) {}
  }
  digitalWrite(PIN_DIR, DIR_FORWARD);
  tActivity = millis();
}

// Feeds out and cuts everything waiting for a cut, then turns the power off: the batch
// is finished, and the servo buzzes while +5V is on. False if stopped by a button.
bool flush() {
  while (numCuts) {
    int32_t n = stepsToCut();
    if (n > 0 && feed(n, V_FAST, stopRequested) < (uint32_t)n) return false;
    if (n <= 0) doCut();
  }
  powerOffAll();
  return true;
}

// ================================================================ printing

// Prints image[0..numLines-1] as a [margin][text][margin] label, schedules its two cuts,
// then feeds it out and cuts it if k = 1.
void printLabel() {
  if (!powerOnAll()) return;
  if (digitalRead(PIN_DIAG)) {
    Serial.println("ERR motor driver fault (DIAG)");
    return;
  }
  v7Min = 99;
  vbusMin = 99;
  uint32_t us = min(heatUs, HEAT_MAX_US);
  uint32_t labelStart = tapePos;
  uint32_t textStart = labelStart + marginUsteps();
  uint32_t labelEnd = textStart + linePosition(numLines) + marginUsteps();
  scheduleCut(labelStart);

  bool ok = true;
  float v = 0;
  if (continuous) {
    // speed: the busiest line must have time to heat all its groups
    // (+ ~20 us loading per group and 10 % margin) before the next one
    uint8_t maxGroups = 0;
    for (uint16_t i = 0; i < numLines; i++) maxGroups = max(maxGroups, numGroups(image[i]));
    float tLine = maxGroups ? maxGroups * (us + 20) * 1.1f : 1000.0f;
    v = constrain(USTEPS_PER_LINE_X100 / 100.0f / (tLine * 1e-6f), (float)V_START, (float)V_CONTINUOUS_MAX);
    job.active = true;
    job.start = textStart;
    job.i = 0;
    job.heating = false;
    job.us = us;
    job.late = 0;
    uint32_t n = labelEnd - tapePos;
    ok = feed(n, v, stopRequested) == n;
    while (job.heating) serviceHeat();
    job.active = false;
  } else {
    for (uint16_t i = 0; ok && i < numLines; i++) {
      if (image[i]) {
        uint32_t target = textStart + linePosition(i);
        uint32_t n = target - tapePos;
        ok = feed(n, n > 40 ? V_FAST : V_LINE, stopRequested) == n && !stopRequested();
        if (ok) heatLine(image[i], us);
      }
      reportProgress(i);
    }
    if (ok) {
      uint32_t n = labelEnd - tapePos;
      ok = feed(n, V_LINE, stopRequested) == n;
    }
  }
  digitalWrite(PIN_OE, HIGH);
  loadBits(0);

  if (!ok) {
    scheduleCut(tapePos);  // the incomplete label will be separated from the next one
    Serial.println("ERR print stopped");
    while (buttonPressed()) {}
    delay(20);
    return;
  }
  scheduleCut(labelEnd);
  if (continuous) Serial.printf("SPEED usteps_s=%.0f ms_line=%.1f late=%u\n", v,
                                USTEPS_PER_LINE_X100 / 100.0f / v * 1000.0f, job.late);
  if (v7Min < 99) Serial.printf("VOLTAGES v7v5_min=%.2f vbus_min=%.2f\n", v7Min, vbusMin);
  // DIAG read before flush(), which turns off the driver and its power
  if (digitalRead(PIN_DIAG)) {
    Serial.println("ERR motor driver fault (DIAG)");
    return;
  }
  if (flushAfter != 0 && !flush()) {
    Serial.println("ERR flush stopped");
    return;
  }
  Serial.println("DONE");
}

// Head -> blade distance calibration: a thin line that the blade must cut through the middle
void calibrate() {
  // flush() turns the power off: turn it back on for the line
  if (!powerOnAll() || !flush() || !powerOnAll()) return;
  uint32_t start = tapePos;
  v7Min = vbusMin = 99;
  for (uint8_t l = 0; l < 3; l++) {
    heatLine(DOT_MASK, heatUs);
    feed(linePosition(l + 1) - (tapePos - start), V_LINE);
  }
  scheduleCut(start + linePosition(1));   // middle of the line
  if (!flush()) return;
  Serial.printf("calibration: blade %.1f mm. The cut must go through the middle of the line.\n",
                settings.headToBladeMm);
  Serial.println("  line on the piece fed out, x mm from the edge           -> blade <value - x>");
  Serial.println("  line on the tape left in the machine, x mm from the edge -> blade <value + x>");
}

// ================================================================ test labels

// 30 x 30 dot square with its diagonal: proportions and orientation
void prepareSquare() {
  for (uint8_t i = 0; i < NUM_DOTS; i++)
    image[i] = (i == 0 || i == NUM_DOTS - 1) ? DOT_MASK
                                             : (1UL | 1UL << (NUM_DOTS - 1) | 1UL << i);
  numLines = NUM_DOTS;
}

// 3 blocks of 40 lines: all dots, stripes of 5 dots, complementary stripes
void prepareBlocks() {
  uint32_t stripes = 0;
  for (uint8_t d = 0; d < NUM_DOTS; d++)
    if ((d / 5) % 2 == 0) stripes |= 1UL << d;
  const uint32_t blocks[] = {DOT_MASK, 0, stripes, 0, DOT_MASK & ~stripes};
  const uint16_t blockLines[] = {40, 10, 40, 10, 40};
  numLines = 0;
  for (uint8_t b = 0; b < 5; b++)
    for (uint16_t l = 0; l < blockLines[b]; l++) image[numLines++] = blocks[b];
}

// Black rectangle from 0 to 6 mm, then a 3-line mark at 10, 15 ... 45 mm
void prepareRuler() {
  const float linePitchMm = USTEPS_PER_LINE_X100 / 100.0 / USTEPS_PER_MM;
  numLines = (uint16_t)(45.0 / linePitchMm + 0.5) + 3;
  for (uint16_t l = 0; l < numLines; l++) {
    bool black = l < 40;
    for (uint8_t mm = 10; mm <= 45; mm += 5) {
      uint16_t start = (uint16_t)(mm / linePitchMm + 0.5);
      if (l >= start && l < start + 3) black = true;
    }
    image[l] = black ? DOT_MASK : 0;
  }
}

// ================================================================ buttons

// Button A held: the tape feeds (accelerates up to V_BUTTON, brakes on release).
// Scheduled cuts along the way are done in passing.
void buttonFeed() {
  delay(20);
  if (digitalRead(PIN_PB1)) return;
  if (!powerOnAll()) {
    while (!digitalRead(PIN_PB1)) {}
    return;
  }
  digitalWrite(PIN_DIR, DIR_FORWARD);
  delayMicroseconds(5);
  const float v02 = (float)V_START * V_START;
  float v = V_START;
  bool held = true;
  while (held || v > V_START) {
    if (held && digitalRead(PIN_PB1)) held = false;
    if (stepsToCut() <= 0) {
      doCut();
      digitalWrite(PIN_DIR, DIR_FORWARD);
      v = V_START;
      if (!held) break;
      continue;
    }
    uint32_t period = (uint32_t)(1e6f / v);
    uint32_t start = micros();
    oneStep();
    tapePos++;
    float v2 = v * v + (held ? 2.0f : -2.0f) * ACCEL;
    v = sqrtf(max(v2, v02));
    v = min(v, (float)V_BUTTON);
    int32_t sc = stepsToCut();
    if (sc < INT32_MAX) v = min(v, sqrtf(v02 + 2.0f * ACCEL * max(sc - 1, (int32_t)0)));
    while (micros() - start < period) {}
  }
  tActivity = millis();
}

// Button B: feeds out and cuts what is printed; if nothing is pending, just cuts
void buttonCut() {
  delay(20);
  if (digitalRead(PIN_PB2)) return;
  while (!digitalRead(PIN_PB2)) {}   // wait for release: a held button would stop the flush
  delay(20);
  if (numCuts) {
    if (powerOnAll()) Serial.println(flush() ? "flushed and cut" : "ERR flush stopped");
  } else {
    cut();
    powerOffAll();
    Serial.println("cut");
  }
  while (buttonPressed()) {}
  delay(20);
}

// ================================================================ commands

void printHelp() {
  Serial.println(F(
    "Printing:\n"
    "  INFO                    head geometry\n"
    "  IMG <n> [us] [c] [k]    n lines, then n times: L <hex>\n"
    "                          c: 1 continuous (default), 0 stop at each line\n"
    "                          k: 1 feed out and cut (default), 0 another label follows\n"
    "  flush                   feed out and cut everything printed\n"
    "  square|blocks|ruler [us] [c]   test labels (fed out and cut)\n"
    "  line <dot> [us]         15 mm line with a single dot (1 = top, 30 = bottom)\n"
    "Settings (saved in memory):\n"
    "  blade [mm]              distance dot row -> blade\n"
    "  margin [mm]             blank before and after the text\n"
    "  group [n]               dots heated at the same time (1 to 30)\n"
    "  calib                   thin line cut through its middle if the blade distance is right\n"
    "Tests:\n"
    "  feed <mm>               feed the tape (negative = reverse)\n"
    "  cut                     one cut, wherever the tape is\n"
    "  servo <us>              servo position, limited to the measured travel\n"
    "  status                  voltages, buttons, DIAG, settings\n"
    "  off                     turn off all power supplies\n"
    "Buttons: A held = feed, B = feed out and cut; a button stops a print"));
}

void printStatus() {
  Serial.printf("VBUS %.2f V | +7V5 %.2f V | +5V %s, +7V5 %s, driver %s | A %s, B %s | DIAG %s\n",
                readVoltage(PIN_VBUS, DIV_VBUS), readVoltage(PIN_V7V5, DIV_7V5),
                on5V ? "ON" : "off", on7V5 ? "ON" : "off", onMot ? "ON" : "off",
                digitalRead(PIN_PB1) ? "released" : "PRESSED",
                digitalRead(PIN_PB2) ? "released" : "PRESSED",
                digitalRead(PIN_DIAG) ? "FAULT" : "ok");
  Serial.printf("blade %.1f mm | margin %.1f mm | group %u | pending cuts %u\n",
                settings.headToBladeMm, settings.marginMm, settings.group, numCuts);
}

// Test labels: [us] [c], always fed out and cut
void setupTest(long us, long c) {
  heatUs = us > 0 ? min((uint32_t)us, HEAT_MAX_US) : HEAT_DEFAULT_US;
  continuous = c != 0;
  flushAfter = 1;
}

// Prints a setting, or changes and saves it if a value is given
void setMm(const char *name, float &value, const char *arg, float maxVal) {
  if (arg[0]) {
    float x = atof(arg);
    if (x < 0 || x > maxVal) {
      Serial.printf("ERR %s: 0 to %.0f mm\n", name, maxVal);
      return;
    }
    value = x;
    saveSettings();
  }
  Serial.printf("%s %.1f mm\n", name, value);
}

void execute(String line) {
  line.trim();
  if (line.length() == 0) return;
  tActivity = millis();

  char cmd[16] = {0};
  char arg1[16] = {0};
  long a2 = -1, a3 = -1, a4 = -1;
  sscanf(line.c_str(), "%15s %15s %ld %ld %ld", cmd, arg1, &a2, &a3, &a4);
  long a1 = arg1[0] ? atol(arg1) : -1;

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
    Serial.printf("INFO first=%u dots=%u pitch_um=%u max_lines=%u cut=2\n",
                  FIRST_DOT, USABLE_DOTS, PITCH_UM, MAX_LINES);
  } else if (!strcasecmp(cmd, "IMG")) {
    if (a1 < 1 || a1 > MAX_LINES) {
      Serial.printf("ERR IMG: 1 to %u lines\n", MAX_LINES);
      return;
    }
    numLines = a1;
    linesReceived = 0;
    heatUs = a2 > 0 ? min((uint32_t)a2, HEAT_MAX_US) : HEAT_DEFAULT_US;
    continuous = a3 != 0;
    flushAfter = a4 == 0 ? 0 : 1;
    receiving = true;
    tLastLine = millis();
    Serial.printf("READY %u\n", numLines);
  } else if (!strcasecmp(cmd, "L")) {
    Serial.println(F("ERR L without IMG"));
  } else if (!strcasecmp(cmd, "FLUSH")) {
    if (powerOnAll()) Serial.println(flush() ? "DONE" : "ERR flush stopped");
  } else if (!strcasecmp(cmd, "square")) {
    setupTest(a1, a2);
    prepareSquare();
    printLabel();
  } else if (!strcasecmp(cmd, "blocks")) {
    setupTest(a1, a2);
    prepareBlocks();
    printLabel();
  } else if (!strcasecmp(cmd, "ruler")) {
    setupTest(a1, a2);
    prepareRuler();
    printLabel();
  } else if (!strcasecmp(cmd, "line") && a1 >= 1 && a1 <= NUM_DOTS) {
    setupTest(a2, 1);
    for (uint8_t i = 0; i < 100; i++) image[i] = 1UL << (a1 - 1);
    numLines = 100;
    printLabel();
  } else if (!strcasecmp(cmd, "blade")) {
    setMm("blade", settings.headToBladeMm, arg1, 200);
  } else if (!strcasecmp(cmd, "margin")) {
    setMm("margin", settings.marginMm, arg1, 50);
  } else if (!strcasecmp(cmd, "group")) {
    if (a1 >= 1 && a1 <= NUM_DOTS) {
      settings.group = a1;
      saveSettings();
    }
    Serial.printf("group %u dots\n", settings.group);
  } else if (!strcasecmp(cmd, "calib")) {
    heatUs = HEAT_DEFAULT_US;
    calibrate();
  } else if (!strcasecmp(cmd, "feed") && arg1[0]) {
    float mm = atof(arg1);
    if (powerOnAll()) {
      long n = lroundf(mm * USTEPS_PER_MM);
      if (n > 0) feed(n, V_FAST, stopRequested);
      else reverse(-n);
      Serial.printf("feed %.1f mm\n", mm);
    }
  } else if (!strcasecmp(cmd, "cut")) {
    cut();
    powerOffAll();
    Serial.println("cut");
  } else if (!strcasecmp(cmd, "servo") && a1 > 0) {
    powerOn5V();
    int us = constrain(a1, SERVO_REST_US, SERVO_CUT_US);
    servo.attach(PIN_CUTTER, SERVO_REST_US, SERVO_CUT_US);
    servo.writeMicroseconds(us);
    delay(600);
    servo.detach();
    Serial.printf("servo %d us\n", us);
  } else if (!strcasecmp(cmd, "status")) {
    printStatus();
  } else if (!strcasecmp(cmd, "off")) {
    powerOffAll();
    Serial.println("power supplies off");
  } else {
    printHelp();
  }
}

// ================================================================ startup and loop

void setup() {
  // Everything off before anything else
  pinMode(PIN_MOT_EN, OUTPUT); digitalWrite(PIN_MOT_EN, HIGH);
  pinMode(PIN_EN_7V5, OUTPUT); digitalWrite(PIN_EN_7V5, LOW);
  pinMode(PIN_EN_5V, OUTPUT);  digitalWrite(PIN_EN_5V, LOW);
  pinMode(PIN_OE, OUTPUT);     digitalWrite(PIN_OE, HIGH);
  pinMode(PIN_SDI, OUTPUT);    digitalWrite(PIN_SDI, LOW);
  pinMode(PIN_CLK, OUTPUT);    digitalWrite(PIN_CLK, LOW);
  pinMode(PIN_LE, OUTPUT);     digitalWrite(PIN_LE, LOW);
  pinMode(PIN_STEP, OUTPUT);   digitalWrite(PIN_STEP, LOW);
  pinMode(PIN_DIR, OUTPUT);    digitalWrite(PIN_DIR, DIR_FORWARD);
  pinMode(PIN_PB1, INPUT_PULLUP);
  pinMode(PIN_PB2, INPUT_PULLUP);
  pinMode(PIN_DIAG, INPUT);
  analogReadResolution(12);
  // First analogRead of a pin: the core then removes the pad's default pull-down.
  // The divider bridges (100k) and their capacitor take a few ms to settle;
  // without this wait, the first VBUS reading was ~1.8 V instead of 5 V.
  analogRead(PIN_V7V5);
  analogRead(PIN_NTC);
  analogRead(PIN_VBUS);
  delay(100);
  loadBits(0);
  loadSettings();

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println(F("Letratag V3"));
  printHelp();
  printStatus();
}

void loop() {
  static String buffer;

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

  if (!receiving) {
    if (!digitalRead(PIN_PB1)) buttonFeed();
    if (!digitalRead(PIN_PB2)) buttonCut();
  }

  if ((on5V || on7V5 || onMot) && !receiving && millis() - tActivity > AUTO_OFF_MS) powerOffAll();
}
