# Ozone Monitor — Firmware README

Implements the functional requirements from `ozone_monitor_requirements_specs.md` v2,
Sections 2 (functional), 4 (calibration), 5 (error handling), and 6 (LCD layout).
Connectivity/web interface (Section 7) is out of scope — deferred, as specified.

## Wiring (from `wiring_diagram.pdf`)

| From | To |
|---|---|
| USB-C 5V → 5V rail | MQ131 `5V`, ESP32 `5V/VIN` |
| GND rail | MQ131 `GND`, ESP32 `GND`, LCD `GND` |
| MQ131 `AO` | → `R1` → node → `R2` → GND (voltage divider) |
| Divider node | → ESP32 `GPIO34` (ADC1_CH6, input-only) |
| MQ131 `DO` | not connected (unused, per spec) |
| ESP32 `GPIO21` | LCD `SDA` |
| ESP32 `GPIO22` | LCD `SCL` |
| ESP32 `3.3V` | LCD `VCC` |

`GPIO34` was a good choice on the diagram: it's on ADC1, which stays usable even once
the (currently deferred) WiFi access point from Section 7 is added — ADC2 pins conflict
with WiFi.

## Required library

- **LiquidCrystal I2C** (Frank de Brabander / Marco Schwartz) — install via
  Arduino IDE → Tools → Manage Libraries.
- If the LCD stays blank, run an I2C scanner sketch to confirm the address
  (the firmware assumes `0x27`; `0x3F` is the other common default) and update
  `LCD_I2C_ADDR` at the top of the sketch.

## What the firmware does

- Samples the MQ131 every 5 s (16x oversampled ADC read), converts to PPB via
  the datasheet-anchored linear model from Section 4.
- 2-minute warm-up countdown on power-up; a short averaging window at the end
  of warm-up establishes the baseline (V0).
- Tracks elapsed time since power-up, min/max PPB with timestamps, and a
  5-reading linear-regression trend (shown as ppb/min, with a small
  stable-band threshold so it doesn't flicker between up/down on noise).
- Two auto-switching LCD pages (2.5 s each, synced to the 5 s sample cycle),
  exactly as laid out in Section 6.
- Detects both error states from Section 5 — ADC pinned at a rail ("no sensor
  reading") and computed PPB outside 10–1000 ("no meaningful reading") — and
  excludes those samples from min/max and the trend.
- Stops sampling and shows a "session complete" screen once the 24 h cap
  (Section 6, item 6) is reached.

## Open items — NOT resolved by this firmware

These are called out in the spec's own Section 9 and still need bench work;
the firmware just uses reasonable defaults so it can run in the meantime:

- **R1/R2 divider values**: assumed **10 kΩ / 15 kΩ** (ratio 0.6) below. That
  caps a full 0–5 V AO swing at 3.0 V into the ADC, leaving headroom under the
  3.3 V limit. Re-check once you've measured the MQ131's actual AO swing in
  your setup, and adjust `DIVIDER_R1_OHM` / `DIVIDER_R2_OHM` accordingly.
- **Current budget** (heater + WiFi + LCD backlight vs. the ESP32 board's 5V
  regulator): not measured or handled in firmware — still an open bench item.
- **ESP32 ADC nonlinearity**: only basic 16x oversampling is used here; no
  external ADC (e.g. ADS1115) is implemented. If readings look noisy or
  nonlinear near the range edges, that's the next thing to revisit.
- **Web interface** (Section 7): deferred entirely, as specified.

## Tuning constants

All the knobs mentioned above live as named constants near the top of
`ozone_monitor.ino` (pin numbers, timing, divider ratio, calibration
reference point, error thresholds, trend window) — no need to hunt through
the logic to adjust them.
