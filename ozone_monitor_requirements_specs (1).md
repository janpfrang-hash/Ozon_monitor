# Ozone (O₃) Concentration Monitor — Requirements & Specifications
*Status: Draft v2 — consolidated from requirements review*

---

## 1. Overview

An ESP32-based device that continuously measures ambient ozone concentration using an MQ131 sensor, displays live readings and statistics on a local 20x4 LCD, and (in a later phase) exposes the same data over a local web interface.

---

## 2. Functional Requirements

1. Sample O₃ concentration every **5 s**, reported in **PPB**.
2. Track and display **elapsed time** since power-up.
3. Track and display **min / max** readings since power-up, each with the elapsed-time **timestamp** at which it occurred.
4. Compute and display a **trend indicator** over the last 5 readings, using **linear regression slope**.
5. On power-up, run a **2-minute warm-up countdown** during which the sensor heats up; no measurements are reported as valid until the countdown completes.
6. Measurement session is capped at **24 hours** maximum (no continuous operation beyond that).
7. Detect and display two distinct **error states**:
   - **No sensor reading** — sensor/wiring fault (e.g. ADC stuck at rail).
   - **No meaningful reading** — value out of the sensor's valid detection range.

---

## 3. Hardware Specifications

### 3.1 Core components
| Component | Detail |
|---|---|
| Gas sensor | MQ131 breakout board (Soldered Electronics or equivalent) |
| Microcontroller | ESP32 dev board |
| Display | 20x4 character LCD with I2C adapter |
| Power | External USB-C 5V supply; MQ131 powered from ESP32's 5V rail where possible |
| IDE | Arduino IDE |

### 3.2 MQ131 sensor — confirmed pinout & characteristics

**Pinout (4 pins, confirmed from board silkscreen):**

| Pin | Function |
|---|---|
| 5V | Power input |
| DO | Digital output (threshold set via on-board "ADJUST" potentiometer) |
| AO | Analog output (voltage varies with gas concentration) |
| GND | Ground |

The board also has an on-board **LED indicator** (lights when DO trips) and an **ADJUST potentiometer** for the DO threshold. These are present on the hardware but **not used** by the current functional requirements (display/logic relies on AO only) — noted as available for future use.

**Sensor technical data:**

| Parameter | Value |
|---|---|
| Detects | Ozone (O₃) |
| Detection range | 10 – 1000 ppb |
| Power supply | 5 V |
| Analog output reference point | ≈1 V at 200 ppb O₃ |
| Dimensions | 22 x 38 mm |

### 3.3 Required interfacing hardware

- **Voltage divider (or buffer) between MQ131 AO and ESP32 ADC input.** The AO signal is referenced to the sensor's 5V supply and can approach that range; the ESP32 ADC's safe input range is 0–3.3V. Direct connection risks damaging the ADC pin.
- Confirm the ESP32 board's onboard 5V regulator has enough current headroom for the sensor heater + WiFi transmit spikes + LCD backlight simultaneously, to avoid brownouts. (Not yet measured — open item.)

---

## 4. Sensor Calibration & PPB Conversion

**Approach: linear approximation (accepted simplification).**

- No pre-use calibration against a reference gas source is required. The device is intended for use in standard rooms without a dedicated O₃ reference; if a test ozone source is used, it is switched on only *after* warm-up/calibration completes.
- At the end of the 2-minute warm-up countdown, the firmware takes a **short averaging window** of AO readings (not a single snapshot) as the baseline (V₀), representing the ambient room air at that moment.
- Concentration is then computed using a **linear model** anchored on the single datasheet reference point (≈1 V at 200 ppb → slope ≈ 5 mV/ppb):

  ```
  ppb = ppb_baseline + (V_measured − V₀) / 0.005
  ```

- **Known limitation (accepted):** this is a linear approximation from one datasheet point. MQ-series sensors are typically non-linear across their full range, so accuracy is expected to be best near 200 ppb and less certain near the range extremes (10 ppb / 1000 ppb). Revisiting this with the full sensor response curve (if/when available) remains a possible future refinement, not required now.

---

## 5. Error Handling

| State | Trigger condition |
|---|---|
| No sensor reading | ADC reading stuck/invalid (e.g. pinned at 0 or full-scale, implausible for a working sensor) |
| No meaningful reading | Computed value < 10 ppb or > 1000 ppb (outside MQ131's rated detection range) |

Both states are shown on the local display status line (see layout below) in place of a numeric reading.

---

## 6. Local Display (20x4 LCD)

**Layout: two pages, auto-switching every 2.5 s** (full cycle = 5 s, synced to the sample interval). Each row uses the **full 20-character width** — no column-splitting.

| Page 1 (shown 0–2.5 s) | Page 2 (shown 2.5–5 s) |
|---|---|
| Current O₃ reading (PPB) | Min value + timestamp |
| Elapsed time since power-up | Max value + timestamp |
| Trend (regression slope, direction + value) | Warm-up / calibration status |
| Status line (OK / error state) | Time remaining in 24h session |

During the 2-minute warm-up countdown, the display shows the countdown instead of the normal pages.

---

## 7. Connectivity & Web Interface — *Deferred*

Not in scope for the current build phase. Captured here for later:

- ESP32 hosts a local WiFi access point.
- **Network will be open (no password).**
- Local webpage (mobile-friendly) showing: current reading, elapsed time, min/max, and a live plot (value vs. time, 5 s steps).
- Implementation notes for later: AP mode means connected phones lose normal internet access, so any charting must be self-contained (no CDN dependency); update mechanism (polling vs. WebSocket) still to be decided at that time.

---

## 8. Data Handling

- With the 24h session cap, elapsed-time tracking via `millis()` does not risk rollover (24h ≪ ~49.7 day overflow limit) — no special handling needed.
- At 5 s sampling over 24h, a full session is 17,280 readings — small enough to buffer entirely in ESP32 RAM if later needed for the web plot (no downsampling required).

---

## 9. Open Items / Risks (not yet resolved)

| Item | Notes |
|---|---|
| Voltage divider component values | Needs to be sized once exact ADC input target is set |
| Heater + WiFi + LCD current budget | Needs a bench measurement against the ESP32 board's regulator rating |
| ESP32 ADC nonlinearity | Not yet decided whether firmware-side oversampling/averaging is sufficient, or an external ADC (e.g. ADS1115) is warranted |
| Full MQ131 response curve | Not used — linear approximation accepted instead (see Section 4) |
| Web interface details | Deferred entirely — see Section 7 |

---

## 10. Change Log

- v1: Initial structured requirements from source document.
- v2: Added warm-up countdown, 24h session cap, 2-page LCD layout (2.5s switch), linear-regression trend, error states, calibration-by-averaging-window, open WiFi (deferred), confirmed MQ131 pinout (5V/DO/AO/GND) and sensor range/output specs, linear PPB approximation.
