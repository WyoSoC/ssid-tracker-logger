# SSID Tracker Logger

A solar-powered Arduino Nano ESP32 field logger for capturing WiFi SSID names during road trips. Built for a humanities project profiling wireless network names across geographic regions.

## Hardware

| Component | Part |
|---|---|
| Microcontroller | Arduino Nano ESP32 (ESP32-S3) |
| GPS | Serial UART GPS module (9600 baud) |
| Display | SSD1306 OLED 128×64 (I2C, 0x3D) |
| Storage | MicroSD card (SPI) |
| Power | LiPo battery + solar panel |
| Button | Momentary pushbutton (D3 to GND) |

### Wiring

```
GPS module
  TX  →  A0  (GPS_RX)
  RX  →  A1  (GPS_TX)
  VCC →  3.3V
  GND →  GND

SSD1306 OLED
  SDA →  A4
  SCL →  A5
  VCC →  3.3V
  GND →  GND

SD card (SPI)
  CS   →  D4
  MOSI →  D11
  MISO →  D12
  SCK  →  D13
  VCC  →  3.3V
  GND  →  GND

Battery voltage monitor (add a voltage divider)
  Battery (+) ─── 100kΩ ─── A2 ─── 100kΩ ─── GND
  A2 reads the midpoint (scales 4.2V max → 2.1V)

Button
  D3 ─── [Button] ─── GND  (uses internal pull-up, no resistor needed)
```

## Arduino Libraries Required

Install via Arduino Library Manager:

- **TinyGPSPlus** — GPS NMEA parsing
- **Adafruit SSD1306** — OLED display driver
- **Adafruit GFX Library** — graphics primitives

The following are bundled with the Arduino ESP32 core (no separate install):

- `WiFi.h`, `WebServer.h`, `Update.h`, `SD.h`, `SPI.h`, `Wire.h`

**Board:** Arduino Nano ESP32 (install via Board Manager → "Arduino ESP32 Boards")

## How It Operates

### Logging Mode (always-on)
- Scans WiFi every 10 seconds (configurable: `SCAN_PERIOD_MS`)
- One CSV row per **location** — location defined as within 10 meters of the anchor point (configurable: `LOCATION_THRESHOLD_M`)
- While stationary: accumulates unique SSID names into the current row
- When moved >10m: finalizes the row, writes to SD, starts a new row
- New daily file created automatically: `YYYYMMDD.csv`
- Timezone detected automatically from GPS longitude (US coverage: HI → AK → PT → MT → CT → ET)

### Button
| Press | Action |
|---|---|
| Short press | Cycle OLED display page |
| Hold 3 seconds | Toggle WiFi Access Point / web server |

### OLED Pages (short press to cycle)
1. **Main** — time, GPS fix, SSID count, battery, SD status
2. **GPS** — lat/lon, HDOP quality, satellite count
3. **WiFi** — SSIDs at current location, total locations today, scan interval
4. **Server** — AP name, IP, auto-off countdown (or instructions to start)

### Web Server (long press to activate)
- AP name: `WyoSSIDLogger` · Password: `soc`
- Connect to the AP, open `http://192.168.4.1`
- Download CSV files by day
- Upload firmware `.bin` for OTA updates (no USB needed in the field)
- Auto-shuts off after 5 minutes of inactivity, resumes logging

## CSV Format

```
local_time, utc_time, lon, lat, satellites, hdop, quality, battery_v, ssid_count, ssids
```

`ssids` is a semicolon-delimited quoted string of unique SSID names observed at that location:

```
2026-06-06 14:23:01,2026-06-06 20:23:01,-104.82012,41.13456,8,1.2,Very Good,3.84,5,"CoffeeShop_Free;XFINITY;ATT-Router-42;NETGEAR87;linksys"
```

## Data Visualization

Hosted at **GitHub Pages** → `docs/` folder.

1. Visit the Pages URL (configure in repo Settings → Pages → source: `/docs`)
2. Click **Load CSV** and select a file downloaded from the device
3. Supports multiple files (multi-day trips — load all at once)

**Features:**
- Interactive map (OpenStreetMap via Leaflet)
- Markers color-coded by SSID density
- Click any marker to see SSID list, GPS quality, and battery
- SSID sidebar with frequency counts — click to filter map to locations where that SSID appears
- Trip stats: total locations, unique SSIDs, date range, estimated distance, average battery

## Configuration

All tunable constants are at the top of `SSID_Tracker_Logger.ino`:

```cpp
#define SCAN_PERIOD_MS         10000UL   // WiFi scan interval (ms)
#define LOCATION_THRESHOLD_M    10.0f   // meters before starting a new row
#define AP_TIMEOUT_MS         300000UL   // web server auto-off (ms)
#define BUTTON_LONG_PRESS_MS    3000    // long press duration (ms)
#define AP_SSID               "WyoSSIDLogger"
#define AP_PASSWORD           "soc"
#define BATTERY_DIVIDER_RATIO   2.0f   // change if you use different resistors
```
