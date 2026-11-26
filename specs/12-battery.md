# 12 — Battery gauge

The display runs from a LiPo cell through the case's rocker switch and charges from its USB-C
port through the board's own charger. Nothing on the board tells the firmware how full the
cell is: the only sign is the charge LED, which turns green when charging ends. This spec adds
a fuel gauge chip on the display's I²C port and a battery icon in the new UI's status bar.
Milestone M10 ([08](08-milestones.md)); decisions D95 to D97.

**State (2026-11-25):** draft. The gauge is ordered. Nothing is built until the hardware check
(§6) has passed on Jeffrey's display.

## 1. Why a fuel gauge

| Option | What it gives | Against it |
|--------|---------------|------------|
| **MAX17048 fuel gauge on I²C (chosen, D95)** | State of charge in percent from the chip's own cell model (Maxim's ModelGauge), the cell voltage, and the charge or discharge rate in %/h. No calibration, no current-sense resistor, no GPIO | A small board and four wires; about $7 of parts |
| Resistor divider into an ADC pin (IO2 or IO8, the only free pins with the SD card in use) | The cell voltage | Voltage alone is a coarse guide: a LiPo's curve is flat in the middle, the voltage sags under the backlight and WiFi, and it reads high while charging. On the forum (FluidTouch thread, posts 350 to 474) a 1 MΩ divider was reported to stop USB charging early; that was never explained, and the same symptom was reported without a divider |
| LED gauge with a push button, no firmware | A reading even with the switch off | Not on the screen |

Upstream has an open pull request for the same chip in the classic UI (jeyeager65/FluidTouch
#46, "Add Battery Meter Support", not merged as of 2026-11-25). Its register reads are the
reference for §3; its findings were a slow charging indication and a full cell reading 92%.

## 2. Hardware

| Part | Use |
|------|-----|
| Adafruit MAX17048 LiPoly/LiIon fuel gauge, product 5580 | The gauge. I²C address 0x36. Two JST-PH battery ports wired in parallel (a pass-through), two STEMMA QT (Qwiic) I²C ports, 10 kΩ pull-ups on SDA and SCL |
| Adafruit cable 4424, JST-PH 4-pin to JST-SH (Qwiic), 200 mm | Display `I2C-OUT` to the gauge's QT port |
| The case's battery, rocker switch and JST-PH leads ([hardware/case/README.md](../hardware/case/README.md)) | Unchanged, except that the gauge goes between the battery and the switch |

```
LiPo ──JST-PH── [MAX17048 board] ──JST-PH── rocker switch ── display BAT
                       │ QT port
                       │ cable 4424
                       └───────────── display I2C-OUT: GND, 3V3, IO15 SDA, IO16 SCL
```

- **The gauge sits before the switch.** The MAX17048 runs from the cell itself, so it stays
  powered with the pendant off and keeps its learned state of charge; it draws tens of
  microamps, which an 8000 mAh cell would take years to give. After the switch it would start
  from scratch at every power-on, and with the switch off and USB in it would read the
  charger's output with no cell behind it.
- **`I2C-OUT`** is printed `IO16_SCL`, `IO15_SDA`, `3V3`, `GND`. Its 3V3 goes to the gauge's
  VIN, which supplies the gauge's I²C side; the pins are open-drain, so the unpowered display
  is not fed from the gauge when the switch is off.
- **Polarity.** The battery must match the + printed beside the gauge's JST port. The
  display's `BAT` plug is reversed against most batteries (case README), so any swap is made
  in the lead from the gauge to the display.
- **The I²C bus** (100 kHz, IO15 and IO16) already carries the touch controller GT911 (0x5D),
  the backlight controller STC8H1K28 (0x30) and the real-time clock (0x51, as printed on the
  board beside the RTC battery). The gauge's 0x36 is free.
- **Charging** still needs the switch on, as today: with it off the cell is disconnected from
  the board's charger.

A wiring page with the schematic, the connection list and the checks before power-on was made
for Jeffrey on 2026-11-25 (private claude.ai artifact, "Pendant fuel gauge wiring"); §2 and §6
here are the record.

## 3. BatteryService

New, `src/services/battery_service.*`. No change to upstream-owned code (02 §2): the service
uses Arduino's `Wire`, which `display_driver.cpp` already starts on the same pins for the
backlight.

| | |
|---|---|
| Start | `begin()` from `LrtApp::begin()`, after the display is up. Address 0x36 is probed and the `VERSION` register (0x08) read; the gauge counts as present when the probe is acknowledged and the version is neither 0x0000 nor 0xFFFF. Not present: the service stays off for the rest of the boot (no retries; a gauge plugged in later is found at the next start) and the serial log says `BatteryService: no gauge at 0x36` |
| Reading | `poll()` from `LrtApp::loop()`, one reading every 10 s: `VCELL` (0x02, 78.125 µV a unit), `SOC` (0x04, the high byte in whole percent, the low byte in 1/256 %), `CRATE` (0x16, signed, 0.208 %/h a unit). Three register reads of 2 bytes each, about 2 ms at 100 kHz |
| Thread | **Main loop only.** The touch driver (LovyanGFX, I²C port 0) and the backlight (`Wire`, also port 0) share the bus and are both used from the main loop; a read from a core 0 task could collide with a touch read. No flash access, so no panel flicker (D44) |
| Percent | `SOC` rounded to whole percent and held to 0 to 100. Shown as the chip reports it. If the hardware check (§6) finds a full cell well under 100%, a scale is decided then |
| Charging | `CRATE` above +1.0 %/h on two readings in a row sets charging; a reading at or below 0 clears it. The chip learns the rate over minutes, so the icon is slow to show charging after USB is plugged in (PR #46 saw the same); the board's charge LED stays the quick sign. Charging full is not shown: the LED's green is the sign |
| Failures | A read that is not acknowledged, or returns fewer than 2 bytes, keeps the last values. After three failed readings in a row the service reports no gauge (the icon goes) and logs it; the next good reading brings it back |
| Gauge settings | Left at the chip's defaults: no `RCOMP` change, no alert thresholds, its own hibernate. Nothing is written to the gauge |
| Interface | `present()`, `percent()`, `volts()`, `ratePerHour()`, `charging()`, `generation()` (changes when the shown values change, as SettingsService's does) |
| Log | One serial line at start and whenever the shown level or charging changes: `BatteryService: 82% 3.98 V +1.2 %/h charging` |

Deep sleep (classic's PowerManager, used by both UIs) needs nothing from the service: the gauge
stays on the cell.

The decoding of the three registers and the level and colour rules (§4) are pure functions,
covered by the host test.

## 4. The battery icon

In the status bar's WiFi column (x 664), on the second row, under the WiFi icon (D96). Nothing
else in the bar moves ([03 §1](03-screens.md#1-frame)).

| Percent | Icon (LVGL symbol, in `lrt_font_18`, which is Montserrat) | Colour |
|---------|-------------------------------------------------------------|--------|
| 80 to 100 | `LV_SYMBOL_BATTERY_FULL` | Light text colour |
| 55 to 79 | `LV_SYMBOL_BATTERY_3` | Light text colour |
| 30 to 54 | `LV_SYMBOL_BATTERY_2` | Light text colour |
| 21 to 29 | `LV_SYMBOL_BATTERY_1` | Light text colour |
| 11 to 20 | `LV_SYMBOL_BATTERY_1` | Amber, as the display's notices (D38) |
| 0 to 10 | `LV_SYMBOL_BATTERY_EMPTY` | Alarm red |
| Charging, any level | `LV_SYMBOL_CHARGE` | Green, the IDLE state's colour |
| No gauge | Nothing; the row stays empty | |

Colour only: no notice in the status bar and no confirm before a job when the battery is low
(D96). The pendant running flat doesn't stop a job (FluidNC goes on cutting) but takes STOP
and PAUSE off the display; the red icon is the warning.

The icon redraws only when `BatteryService::generation()` changes, so the 10 s reading
doesn't invalidate the bar on its own.

**Settings > Tools > About** gets a line "Battery 82%, 3.98 V, charging" (or "discharging
1.2 %/h", or "No battery gauge found") for the exact values.

**Classic UI:** unchanged (D97).

## 5. Simulator

The simulator's `Wire` stub acknowledges every address and returns no data, so the service
finds no gauge there and the row stays empty. A `sim:` commit adds a model of the gauge at
0x36 to the stub (only that address; others behave as now) with its registers set from:

- a command-line option `--battery <percent>[,charging]`; without it there is no gauge;
- a script command `battery <percent> [charging]` and `battery none`, for scripted changes.

Checked in the simulator: each row of §4's table, the change while running, the gauge going
and coming back (`battery none`), and the About line.

## 6. Hardware check (Jeffrey, before building)

1. **Cable pin order.** The 4424 cable is GND, V+, SDA, SCL from pin 1 (black, red, blue,
   yellow); `I2C-OUT` is printed in the reverse order, so it depends on which end is pin 1.
   With the cable in the display only and the display on: black to red reads 3.3 V. If yellow
   to blue reads it instead, reverse the four wires in the PH housing.
2. **Polarity** of both JST leads as §2; with the switch on, 3.6 to 4.2 V at the display's
   `BAT` pins, + where the PCB says +.
3. **The gauge is found.** The Advance's boot code scans the bus and logs
   `Found device at 0x..` (`display_driver.cpp`); the list shows 0x36 with 0x30, 0x51 and 0x5D.
4. **Touch works as before** with the gauge's pull-ups added to the bus.

Then, once the service is built (§7): the voltage in About within 0.05 V of a multimeter at the
cell; the percent after a full charge (the green LED), which decides whether a scale is needed
(§3); how long after plugging in USB the icon shows charging; the cell's voltage after a day
with the switch off, against the day before.

## 7. Build order

Branch `lrt/m10-battery` from `lrt/main`, after the hardware check.

1. `sim:` the gauge model in the `Wire` stub (§5).
2. `services:` BatteryService (§3), with the host test of the decoding and the level rules.
3. `app:` the icon (§4) and the About line.

Done when: in the simulator, every row of §4 and the About line; on the device, the checks after
the build in §6, with the results written into this section.
