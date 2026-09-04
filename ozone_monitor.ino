/*
  Ozone (O3) Concentration Monitor — ESP32 + MQ131 + 20x4 I2C LCD
  =================================================================
  Implements the functional requirements from
  ozone_monitor_requirements_specs.md (v2), Sections 2, 4, 5, 6.

  WIRING (from wiring_diagram.pdf):
  -----------------------------------------------------------------
    MQ131 AO --[R1]--+--[R2]-- GND        (voltage divider)
                      |
                    ESP32 GPIO34 (ADC1_CH6, input-only)

    MQ131  5V, GND  -> 5V rail, GND rail (from USB-C 5V input)
    ESP32  5V/VIN, GND -> 5V rail, GND rail
    ESP32  GPIO21 (SDA) -> LCD SDA
    ESP32  GPIO22 (SCL) -> LCD SCL
    LCD    3.3V, GND -> ESP32 3.3V, GND rail
    MQ131  DO        -> not connected (unused per spec; DO/ADJUST
                         pot are present on the board but not wired)

  REQUIRED LIBRARY (Arduino Library Manager):
    - "LiquidCrystal I2C" (Frank de Brabander / Marco Schwartz)

  OPEN ITEMS *NOT* RESOLVED BY THIS FIRMWARE (spec Section 9 — still
  need bench work, this code just needs reasonable defaults to run):
    - R1/R2 exact values: 10k/15k assumed below (ratio 0.6), giving a
      5V AO swing -> max 3.0V at the ADC pin, i.e. headroom under the
      3.3V limit. Re-check once actual AO swing is measured.
    - Heater + WiFi + LCD current budget: not measured/handled here.
    - ESP32 ADC nonlinearity: only basic oversampling is used; no
      external ADC (e.g. ADS1115) is implemented.
    - Web interface (spec Section 7): out of scope, deferred.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>

// ---------------------------------------------------------------------
// Pin assignment (per wiring_diagram.pdf)
// ---------------------------------------------------------------------
static const uint8_t PIN_MQ131_AO = 34;   // ADC1_CH6, input-only pin
static const uint8_t PIN_I2C_SDA  = 21;
static const uint8_t PIN_I2C_SCL  = 22;

// ---------------------------------------------------------------------
// LCD configuration
// ---------------------------------------------------------------------
static const uint8_t LCD_I2C_ADDR = 0x27;  // try 0x3F if the display stays blank
static const uint8_t LCD_COLS = 20;
static const uint8_t LCD_ROWS = 4;
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

// ---------------------------------------------------------------------
// Timing constants (spec Section 2)
// ---------------------------------------------------------------------
static const unsigned long WARMUP_DURATION_MS = 2UL * 60UL * 1000UL;         // 2 min
static const unsigned long SAMPLE_INTERVAL_MS = 5000UL;                      // 5 s
static const unsigned long PAGE_SWITCH_MS     = 2500UL;                      // 2.5 s
static const unsigned long SESSION_MAX_MS     = 24UL * 60UL * 60UL * 1000UL; // 24 h

// ---------------------------------------------------------------------
// ADC / voltage divider (spec Section 3.3; values are an open item —
// see header comment)
// ---------------------------------------------------------------------
static const float ADC_VREF         = 3.3f;
static const int   ADC_MAX_COUNTS   = 4095;
static const int   ADC_OVERSAMPLE_N = 16;

// R1 = AO -> node, R2 = node -> GND, node -> GPIO34 (see wiring diagram)
static const float DIVIDER_R1_OHM = 10000.0f;
static const float DIVIDER_R2_OHM = 15000.0f;
static const float DIVIDER_RATIO  = DIVIDER_R2_OHM / (DIVIDER_R1_OHM + DIVIDER_R2_OHM); // Vnode / Vsensor

// ADC "stuck at rail" thresholds -> "No sensor reading" error (spec Section 5)
static const int ADC_RAIL_LOW_COUNTS  = 10;
static const int ADC_RAIL_HIGH_COUNTS = 4085;

// ---------------------------------------------------------------------
// MQ131 linear calibration model (spec Section 4)
// ---------------------------------------------------------------------
static const float MQ131_REF_VOLTAGE     = 1.0f;    // V, at reference point
static const float MQ131_REF_PPB         = 200.0f;  // ppb, at reference point
static const float MQ131_SLOPE_V_PER_PPB = 0.005f;  // 5 mV/ppb
static const float MQ131_RANGE_MIN_PPB   = 10.0f;
static const float MQ131_RANGE_MAX_PPB   = 1000.0f;

static const int BASELINE_SAMPLE_COUNT           = 20;
static const unsigned long BASELINE_SAMPLE_SPACING_MS = 100UL;

// ---------------------------------------------------------------------
// Trend indicator (spec Section 2, item 4)
// ---------------------------------------------------------------------
static const int   TREND_WINDOW = 5;
static const float TREND_STABLE_THRESHOLD_PPB_PER_MIN = 0.5f;

// ---------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------
enum SystemState { STATE_WARMUP, STATE_RUNNING, STATE_SESSION_COMPLETE };
enum ErrorState  { ERR_OK, ERR_NO_SENSOR, ERR_NO_MEANINGFUL };

SystemState systemState = STATE_WARMUP;
ErrorState  currentError = ERR_OK;

unsigned long powerUpMillis    = 0;
unsigned long lastSampleMillis = 0;
bool sampledOnce = false;
bool forceRedraw = true;

float baselineVoltage = 0.0f;  // V0, sensor-side volts (post-divider, corrected)
float baselinePPB     = 0.0f;

float currentPPB = 0.0f;

bool haveMinMax = false;
float minPPB = 0.0f; unsigned long minTimestampMs = 0;
float maxPPB = 0.0f; unsigned long maxTimestampMs = 0;

float trendBuffer[TREND_WINDOW];
int   trendCount = 0;
float trendSlopePpbPerMin = 0.0f;
bool  trendValid = false;

// ---------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MQ131_AO, ADC_11db); // ~0-3.3V usable range on GPIO34

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcdLine(0, "O3 Concentration");
  lcdLine(1, "Monitor");
  lcdLine(2, "Starting up...");
  lcdLine(3, "");
  delay(1500);

  powerUpMillis = millis();
}

// ---------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------
void loop() {
  unsigned long now = millis();
  unsigned long elapsed = now - powerUpMillis; // elapsed time since power-up

  if (systemState == STATE_WARMUP) {
    handleWarmup(elapsed);
  } else if (systemState == STATE_RUNNING) {
    if (elapsed >= SESSION_MAX_MS) {
      systemState = STATE_SESSION_COMPLETE;
      forceRedraw = true;
    } else {
      if (!sampledOnce || (now - lastSampleMillis >= SAMPLE_INTERVAL_MS)) {
        lastSampleMillis = now;
        sampledOnce = true;
        takeSample(elapsed);
        forceRedraw = true;
      }
      updateDisplayRunning(elapsed);
    }
  }

  if (systemState == STATE_SESSION_COMPLETE) {
    renderSessionComplete();
  }

  delay(50);
}

// ---------------------------------------------------------------------
// Warm-up countdown (spec Section 2, item 5)
// ---------------------------------------------------------------------
void handleWarmup(unsigned long elapsed) {
  if (elapsed >= WARMUP_DURATION_MS) {
    establishBaseline();
    systemState = STATE_RUNNING;
    lastSampleMillis = millis();
    sampledOnce = true;
    takeSample(millis() - powerUpMillis); // first valid sample right after warm-up
    forceRedraw = true;
    return;
  }

  unsigned long remainingMs  = WARMUP_DURATION_MS - elapsed;
  unsigned long remainingSec = remainingMs / 1000UL;
  int mm = remainingSec / 60;
  int ss = remainingSec % 60;

  char line[LCD_COLS + 1];
  lcdLine(0, "O3 Monitor");
  lcdLine(1, "Sensor warming up");
  snprintf(line, sizeof(line), "Ready in: %02d:%02d", mm, ss);
  lcdLine(2, line);
  lcdLine(3, "Please wait...");
}

void establishBaseline() {
  // Short averaging window (not a single snapshot) taken as V0 (spec Section 4)
  uint32_t sum = 0;
  for (int i = 0; i < BASELINE_SAMPLE_COUNT; i++) {
    sum += readRawADCOversampled();
    delay(BASELINE_SAMPLE_SPACING_MS);
  }
  int rawAvg = (int)(sum / BASELINE_SAMPLE_COUNT);
  baselineVoltage = rawToSensorVoltage(rawAvg);
  baselinePPB     = voltageToPPB(baselineVoltage);
}

// ---------------------------------------------------------------------
// Sampling (spec Sections 2 item 1, 4, 5)
// ---------------------------------------------------------------------
int readRawADCOversampled() {
  uint32_t sum = 0;
  for (int i = 0; i < ADC_OVERSAMPLE_N; i++) {
    sum += analogRead(PIN_MQ131_AO);
    delayMicroseconds(200);
  }
  return (int)(sum / ADC_OVERSAMPLE_N);
}

// Raw ADC counts -> sensor-side AO voltage (undoes the R1/R2 divider)
float rawToSensorVoltage(int raw) {
  float vAdc = (raw / (float)ADC_MAX_COUNTS) * ADC_VREF;
  return vAdc / DIVIDER_RATIO;
}

// Datasheet-anchored linear model (spec Section 4), applied to the fixed
// reference point. Used once, at warm-up end, to derive ppb_baseline from
// V0 (see establishBaseline()) -- not used directly for live samples.
float voltageToPPB(float vSensor) {
  return MQ131_REF_PPB + (vSensor - MQ131_REF_VOLTAGE) / MQ131_SLOPE_V_PER_PPB;
}

// Baseline-relative form actually used for live samples (spec Section 4):
//   ppb = ppb_baseline + (V_measured - V0) / slope
// Numerically this is algebraically equivalent to voltageToPPB(vSensor)
// once ppb_baseline is itself derived from V0 via that same function --
// but it's kept as an explicit offset from (V0, ppb_baseline) rather than
// inlined, so a future refinement (e.g. periodic re-baselining, or
// replacing the single-point datasheet line with a fuller response curve)
// only has to change how V0/ppb_baseline are derived, not this function.
float sampleToPPB(float vSensor) {
  return baselinePPB + (vSensor - baselineVoltage) / MQ131_SLOPE_V_PER_PPB;
}

void takeSample(unsigned long elapsed) {
  int raw = readRawADCOversampled();

  // "No sensor reading" - ADC pinned at a rail, implausible for a working sensor
  if (raw <= ADC_RAIL_LOW_COUNTS || raw >= ADC_RAIL_HIGH_COUNTS) {
    currentError = ERR_NO_SENSOR;
    Serial.println(F("Sample: NO SENSOR READING (ADC at rail)"));
    return;
  }

  float vSensor = rawToSensorVoltage(raw);
  float ppb = sampleToPPB(vSensor);

  // "No meaningful reading" - outside MQ131's rated detection range
  if (ppb < MQ131_RANGE_MIN_PPB || ppb > MQ131_RANGE_MAX_PPB) {
    currentError = ERR_NO_MEANINGFUL;
    Serial.print(F("Sample: NO MEANINGFUL READING, computed ppb="));
    Serial.println(ppb);
    return;
  }

  currentError = ERR_OK;
  currentPPB = ppb;
  updateMinMax(ppb, elapsed);
  pushTrend(ppb);

  Serial.print(F("Sample: "));
  Serial.print(ppb);
  Serial.println(F(" ppb"));
}

void updateMinMax(float ppb, unsigned long elapsed) {
  if (!haveMinMax) {
    minPPB = maxPPB = ppb;
    minTimestampMs = maxTimestampMs = elapsed;
    haveMinMax = true;
    return;
  }
  if (ppb < minPPB) { minPPB = ppb; minTimestampMs = elapsed; }
  if (ppb > maxPPB) { maxPPB = ppb; maxTimestampMs = elapsed; }
}

// Keeps the last TREND_WINDOW valid readings in temporal order and
// recomputes the least-squares slope once the window is full.
void pushTrend(float ppb) {
  if (trendCount < TREND_WINDOW) {
    trendBuffer[trendCount++] = ppb;
  } else {
    for (int i = 1; i < TREND_WINDOW; i++) trendBuffer[i - 1] = trendBuffer[i];
    trendBuffer[TREND_WINDOW - 1] = ppb;
  }
  if (trendCount == TREND_WINDOW) {
    trendSlopePpbPerMin = computeTrendSlopePpbPerMin();
    trendValid = true;
  }
}

// Simple linear regression slope over the last TREND_WINDOW samples
// (x = sample index, spaced SAMPLE_INTERVAL_MS apart), converted to ppb/min.
float computeTrendSlopePpbPerMin() {
  const int n = TREND_WINDOW;
  float xbar = (n - 1) / 2.0f;
  float ybar = 0.0f;
  for (int i = 0; i < n; i++) ybar += trendBuffer[i];
  ybar /= n;

  float num = 0.0f, den = 0.0f;
  for (int i = 0; i < n; i++) {
    float dx = i - xbar;
    num += dx * (trendBuffer[i] - ybar);
    den += dx * dx;
  }
  float slopePerSample = (den != 0.0f) ? (num / den) : 0.0f;
  float samplesPerMinute = 60000.0f / (float)SAMPLE_INTERVAL_MS;
  return slopePerSample * samplesPerMinute;
}

// ---------------------------------------------------------------------
// Display (spec Section 6)
// ---------------------------------------------------------------------

// Writes exactly LCD_COLS characters (space-padded/truncated) so every
// row uses the full width and old content is always overwritten cleanly.
void lcdLine(uint8_t row, const char *text) {
  char buf[LCD_COLS + 1];
  snprintf(buf, sizeof(buf), "%-20.20s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void formatElapsed(unsigned long ms, char *out, size_t outLen) {
  unsigned long totalSec = ms / 1000UL;
  unsigned long hh = totalSec / 3600UL;
  unsigned long mm = (totalSec % 3600UL) / 60UL;
  unsigned long ss = totalSec % 60UL;
  snprintf(out, outLen, "%02lu:%02lu:%02lu", hh, mm, ss);
}

// Pages auto-switch every 2.5s within each 5s sample cycle (spec Section 6),
// only redrawing when the page changes or a new sample just came in.
void updateDisplayRunning(unsigned long elapsed) {
  static int lastPage = -1;
  int page = ((elapsed % SAMPLE_INTERVAL_MS) < PAGE_SWITCH_MS) ? 0 : 1;

  if (page != lastPage || forceRedraw) {
    if (page == 0) renderPage1(elapsed);
    else renderPage2(elapsed);
    lastPage = page;
    forceRedraw = false;
  }
}

void renderPage1(unsigned long elapsed) {
  char line[LCD_COLS + 1];
  char timeStr[9];

  // Row 1: current O3 reading
  if (currentError == ERR_NO_SENSOR || currentError == ERR_NO_MEANINGFUL) {
    lcdLine(0, "O3: ---- ppb");
  } else {
    snprintf(line, sizeof(line), "O3: %4.0f ppb", currentPPB);
    lcdLine(0, line);
  }

  // Row 2: elapsed time since power-up
  formatElapsed(elapsed, timeStr, sizeof(timeStr));
  snprintf(line, sizeof(line), "Elapsed: %s", timeStr);
  lcdLine(1, line);

  // Row 3: trend (linear regression slope over last 5 readings)
  if (!trendValid) {
    lcdLine(2, "Trend: gathering...");
  } else if (fabs(trendSlopePpbPerMin) < TREND_STABLE_THRESHOLD_PPB_PER_MIN) {
    lcdLine(2, "Trend: STABLE");
  } else if (trendSlopePpbPerMin > 0) {
    snprintf(line, sizeof(line), "Trend: UP %.1f/min", trendSlopePpbPerMin);
    lcdLine(2, line);
  } else {
    snprintf(line, sizeof(line), "Trend: DN %.1f/min", -trendSlopePpbPerMin);
    lcdLine(2, line);
  }

  // Row 4: status line (OK / error state)
  switch (currentError) {
    case ERR_OK:            lcdLine(3, "Status: OK"); break;
    case ERR_NO_SENSOR:     lcdLine(3, "Status: NO SENSOR"); break;
    case ERR_NO_MEANINGFUL: lcdLine(3, "Status: OUT OF RANGE"); break;
  }
}

void renderPage2(unsigned long elapsed) {
  char line[LCD_COLS + 1];
  char timeStr[9];

  // Row 1: min value + timestamp
  if (haveMinMax) {
    formatElapsed(minTimestampMs, timeStr, sizeof(timeStr));
    snprintf(line, sizeof(line), "Min:%4.0fppb %s", minPPB, timeStr);
  } else {
    snprintf(line, sizeof(line), "Min: no data yet");
  }
  lcdLine(0, line);

  // Row 2: max value + timestamp
  if (haveMinMax) {
    formatElapsed(maxTimestampMs, timeStr, sizeof(timeStr));
    snprintf(line, sizeof(line), "Max:%4.0fppb %s", maxPPB, timeStr);
  } else {
    snprintf(line, sizeof(line), "Max: no data yet");
  }
  lcdLine(1, line);

  // Row 3: warm-up / calibration status
  snprintf(line, sizeof(line), "Baseline set %.2fV", baselineVoltage);
  lcdLine(2, line);

  // Row 4: time remaining in the 24h session
  unsigned long remaining = (elapsed < SESSION_MAX_MS) ? (SESSION_MAX_MS - elapsed) : 0;
  formatElapsed(remaining, timeStr, sizeof(timeStr));
  snprintf(line, sizeof(line), "Sess left %s", timeStr);
  lcdLine(3, line);
}

void renderSessionComplete() {
  static bool rendered = false;
  if (rendered) return;

  char line[LCD_COLS + 1];
  lcdLine(0, "SESSION COMPLETE");
  snprintf(line, sizeof(line), "Min:%4.0f Max:%4.0f", minPPB, maxPPB);
  lcdLine(1, line);
  lcdLine(2, "24h limit reached");
  lcdLine(3, "Power-cycle to redo");

  rendered = true;
  Serial.println(F("Session complete (24h limit reached)."));
}
