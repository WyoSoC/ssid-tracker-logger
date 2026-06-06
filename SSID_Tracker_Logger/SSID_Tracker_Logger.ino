#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <Update.h>
#include <time.h>
#include <sys/time.h>

// ======================================================
// CONFIGURATION — edit these to change behavior
// ======================================================

// Hardware pins
#define GPS_RX          A0
#define GPS_TX          A1
#define OLED_SDA        A4
#define OLED_SCL        A5
#define SD_CS           D4
#define SD_MOSI         D11
#define SD_MISO         D12
#define SD_SCK          D13
#define BATTERY_PIN     A2   // midpoint of 100kΩ/100kΩ voltage divider from battery+
#define BUTTON_PIN      D3

// OLED
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDR       0x3D

// Access point credentials
#define AP_SSID         "WyoSSIDLogger"
#define AP_PASSWORD     "soc"

// Timing (milliseconds) — adjust freely
#define SCAN_PERIOD_MS         10000UL   // WiFi scan interval
#define CLOCK_SYNC_PERIOD_MS   10000UL   // GPS clock re-sync interval
#define AP_TIMEOUT_MS         300000UL   // web server auto-off (5 min)
#define DISPLAY_UPDATE_MS       1000UL   // OLED refresh

// Behavior
#define LOCATION_THRESHOLD_M    10.0f   // meters to qualify as a new location row
#define BATTERY_SAMPLES         10      // ADC reads averaged per battery reading
#define BATTERY_DIVIDER_RATIO   2.0f    // (R_top+R_bot)/R_bot — 100k/100k = 2.0
#define BUTTON_LONG_PRESS_MS    3000    // ms held to count as long press
#define MAX_SSIDS_PER_LOC       64      // unique SSIDs stored per location
#define SSID_FIELD_LEN          33      // 32-char max SSID + null terminator

// ======================================================
// DATA TYPES
// ======================================================

struct LocationRecord {
  char   local_time[24];
  char   utc_time[24];
  double lat;
  double lon;
  int    satellites;
  float  hdop;
  float  battery_v;
  char   ssids[MAX_SSIDS_PER_LOC][SSID_FIELD_LEN];
  int    ssid_count;
  bool   valid;
};

enum SystemMode  { MODE_LOGGING, MODE_WEBSERVER };
enum DisplayPage { PAGE_MAIN, PAGE_GPS, PAGE_WIFI, PAGE_SERVER, PAGE_COUNT };

// ======================================================
// GLOBALS
// ======================================================

TinyGPSPlus      gps;
HardwareSerial   gpsSerial(1);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer        server(80);

SystemMode   systemMode  = MODE_LOGGING;
DisplayPage  displayPage = PAGE_MAIN;

unsigned long lastScan     = 0;
unsigned long lastDisplay  = 0;
unsigned long lastClockSet = 0;
unsigned long apStartTime  = 0;

bool gpsClockSet = false;
bool sdReady     = false;

LocationRecord currentRecord;
int            totalLoggedLocations = 0;

bool          buttonWasPressed = false;
unsigned long buttonPressTime  = 0;
bool          longPressHandled = false;

// ======================================================
// FORWARD DECLARATIONS
// ======================================================

const char* hdopQuality(float h);
bool        isLeapYear(int y);
int         daysInMonth(int y, int m);
time_t      makeEpochUTC(int yr, int mo, int dy, int hr, int mn, int sc);
void        updateDisplay();
void        startWebServer();
void        stopWebServer();

// ======================================================
// TIMEZONE — longitude-based approximation (US + territories)
// Boundary is geographic center of each zone; accurate for road trips,
// may be off by one zone near state borders that deviate from meridians.
// ======================================================

const char* getTimezoneForCoords(double /*lat*/, double lon) {
  if (lon < -154.0) return "HST10";                        // Hawaii
  if (lon < -130.0) return "AKST9AKDT,M3.2.0,M11.1.0";    // Alaska
  if (lon < -114.5) return "PST8PDT,M3.2.0,M11.1.0";      // Pacific
  if (lon < -101.5) return "MST7MDT,M3.2.0,M11.1.0";      // Mountain
  if (lon <  -87.5) return "CST6CDT,M3.2.0,M11.1.0";      // Central
  return "EST5EDT,M3.2.0,M11.1.0";                         // Eastern
}

// ======================================================
// BATTERY
// Hardware: 100kΩ from battery+ to A2, 100kΩ from A2 to GND.
// Scales 4.2V max → 2.1V at ADC input (within 3.3V rail).
// Uses ADC1 channel — safe when WiFi is active on ESP32-S3.
// ======================================================

float readBatteryVoltage() {
  long sum = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    sum += analogRead(BATTERY_PIN);
    delayMicroseconds(500);
  }
  return ((float)sum / BATTERY_SAMPLES) / 4095.0f * 3.3f * BATTERY_DIVIDER_RATIO;
}

// ======================================================
// LOCATION HELPERS
// ======================================================

float distanceMeters(double lat1, double lon1, double lat2, double lon2) {
  float dx = (float)(lon2 - lon1) * cosf((float)lat1 * DEG_TO_RAD) * 111320.0f;
  float dy = (float)(lat2 - lat1) * 111320.0f;
  return sqrtf(dx * dx + dy * dy);
}

bool addUniqueSSID(LocationRecord &rec, const String &ssid) {
  if (ssid.length() == 0 || rec.ssid_count >= MAX_SSIDS_PER_LOC) return false;
  for (int i = 0; i < rec.ssid_count; i++) {
    if (ssid.equals(rec.ssids[i])) return false;
  }
  ssid.toCharArray(rec.ssids[rec.ssid_count++], SSID_FIELD_LEN);
  return true;
}

void resetRecord(LocationRecord &rec) {
  memset(&rec, 0, sizeof(LocationRecord));
}

// ======================================================
// TIME HELPERS
// ======================================================

void getLocalDateTime(char *buf, size_t len) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  strftime(buf, len, "%Y-%m-%d %H:%M:%S", &t);
}

void getLocalTimeOnly(char *buf, size_t len) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  strftime(buf, len, "%H:%M:%S", &t);
}

void getUtcDateTime(char *buf, size_t len) {
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  strftime(buf, len, "%Y-%m-%d %H:%M:%S", &t);
}

// ======================================================
// GPS CLOCK SYNC
// ======================================================

void updateSystemClockFromGPS() {
  if (!gps.time.isValid() || !gps.date.isValid()) return;
  if (gps.date.year() < 2024) return;
  if (gpsClockSet && millis() - lastClockSet < CLOCK_SYNC_PERIOD_MS) return;

  if (gps.location.isValid()) {
    setenv("TZ", getTimezoneForCoords(gps.location.lat(), gps.location.lng()), 1);
    tzset();
  }

  time_t epoch = makeEpochUTC(
    gps.date.year(), gps.date.month(), gps.date.day(),
    gps.time.hour(),  gps.time.minute(),  gps.time.second()
  );
  struct timeval tv = { epoch, (suseconds_t)(gps.time.centisecond() * 10000) };
  settimeofday(&tv, nullptr);

  gpsClockSet  = true;
  lastClockSet = millis();
  Serial.println("System clock synced from GPS.");
}

bool gpsReadyForLogging() {
  return gpsClockSet
      && gps.location.isValid()
      && gps.date.isValid()
      && gps.time.isValid()
      && gps.date.year() >= 2024;
}

// ======================================================
// SD LOGGING
// ======================================================

String getDailyFilename() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char name[24];
  snprintf(name, sizeof(name), "/%04d%02d%02d.csv",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return String(name);
}

void ensureCsvHeader(const String &filename) {
  if (SD.exists(filename)) return;
  File f = SD.open(filename, FILE_WRITE);
  if (!f) return;
  f.println("local_time,utc_time,lon,lat,satellites,hdop,quality,battery_v,ssid_count,ssids");
  f.close();
}

void writeRecordToSD(const LocationRecord &rec) {
  if (!sdReady || !rec.valid || rec.ssid_count == 0) return;

  String filename = getDailyFilename();
  ensureCsvHeader(filename);

  File f = SD.open(filename, FILE_APPEND);
  if (!f) { Serial.println("SD open failed"); return; }

  f.print(rec.local_time);        f.print(',');
  f.print(rec.utc_time);          f.print(',');
  f.print(rec.lon, 6);            f.print(',');
  f.print(rec.lat, 6);            f.print(',');
  f.print(rec.satellites);        f.print(',');
  f.print(rec.hdop, 2);           f.print(',');
  f.print(hdopQuality(rec.hdop)); f.print(',');
  f.print(rec.battery_v, 3);      f.print(',');
  f.print(rec.ssid_count);        f.print(',');

  // SSIDs as a semicolon-delimited quoted string
  f.print('"');
  for (int i = 0; i < rec.ssid_count; i++) {
    if (i > 0) f.print(';');
    for (const char *p = rec.ssids[i]; *p; p++) {
      if (*p == '"') f.print('"'); // CSV double-quote escape
      f.print(*p);
    }
  }
  f.println('"');
  f.close();

  totalLoggedLocations++;
  Serial.printf("Wrote loc #%d: %d SSIDs  (%.5f, %.5f)\n",
                totalLoggedLocations, rec.ssid_count, rec.lat, rec.lon);
}

// ======================================================
// WIFI SCAN + LOCATION UPDATE
// ======================================================

void scanWiFiAndUpdate() {
  Serial.println("WiFi scan...");
  int n = WiFi.scanNetworks(false, true); // synchronous, show hidden
  if (n <= 0) { WiFi.scanDelete(); return; }

  bool   gpsValid = gps.location.isValid();
  double curLat   = gpsValid ? gps.location.lat() : 0.0;
  double curLon   = gpsValid ? gps.location.lng() : 0.0;

  bool locationChanged = currentRecord.valid && gpsValid
    && distanceMeters(currentRecord.lat, currentRecord.lon, curLat, curLon) > LOCATION_THRESHOLD_M;

  if (locationChanged || !currentRecord.valid) {
    if (currentRecord.valid) writeRecordToSD(currentRecord);
    resetRecord(currentRecord);
    currentRecord.valid      = true;
    currentRecord.lat        = curLat;
    currentRecord.lon        = curLon;
    currentRecord.satellites = gps.satellites.isValid() ? (int)gps.satellites.value() : -1;
    currentRecord.hdop       = gps.hdop.isValid() ? gps.hdop.hdop() : -1.0f;
    currentRecord.battery_v  = readBatteryVoltage();
    getLocalDateTime(currentRecord.local_time, sizeof(currentRecord.local_time));
    getUtcDateTime(currentRecord.utc_time,     sizeof(currentRecord.utc_time));
  }

  int added = 0;
  for (int i = 0; i < n; i++) {
    if (addUniqueSSID(currentRecord, WiFi.SSID(i))) added++;
  }
  WiFi.scanDelete();
  Serial.printf("  +%d new SSIDs  (total %d at this location)\n", added, currentRecord.ssid_count);
}

// ======================================================
// WEB SERVER — HTML builder
// ======================================================

String buildStatusPage() {
  String h;
  h.reserve(2048);

  h = F("<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>SSID Logger</title>"
    "<style>"
    "body{background:#111;color:#ccc;font-family:monospace;margin:20px;max-width:800px}"
    "h1{color:#0f0;margin-bottom:2px}"
    "h2{color:#0af;margin-top:24px;margin-bottom:8px}"
    "a,a:visited{color:#0af}"
    ".card{background:#1a1a1a;border:1px solid #333;padding:12px;margin:10px 0;border-radius:3px}"
    "table{border-collapse:collapse;width:100%}"
    "td,th{border:1px solid #333;padding:7px 10px;text-align:left}"
    "th{color:#0af;background:#161616}"
    ".btn{background:#0f0;color:#111;padding:6px 14px;border:none;cursor:pointer;"
         "font-family:monospace;font-weight:bold;text-decoration:none;"
         "display:inline-block;margin:2px;border-radius:2px}"
    ".warn{color:#f80}.ok{color:#0f0}"
    "input[type=file]{color:#0f0;margin-bottom:10px}"
    "p{margin:6px 0}"
    "</style></head><body>"
    "<h1>&#9632; SSID Logger</h1>");

  // Live status card
  h += F("<div class='card'>");
  if (gps.location.isValid()) {
    char buf[80];
    snprintf(buf, sizeof(buf), "FIX &nbsp;%.5f, %.5f &nbsp; %d sats &nbsp; HDOP %.1f (%s)",
             gps.location.lat(), gps.location.lng(),
             gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
             gps.hdop.isValid() ? gps.hdop.hdop() : -1.0f,
             hdopQuality(gps.hdop.isValid() ? gps.hdop.hdop() : -1.0f));
    h += "<p><b>GPS:</b> <span class='ok'>" + String(buf) + "</span></p>";
  } else {
    h += F("<p><b>GPS:</b> <span class='warn'>No Fix</span></p>");
  }

  char bvBuf[8];
  dtostrf(readBatteryVoltage(), 4, 2, bvBuf);
  h += "<p><b>Battery:</b> " + String(bvBuf) + " V</p>";
  h += "<p><b>Locations logged today:</b> " + String(totalLoggedLocations) + "</p>";
  h += "<p><b>SSIDs at current location:</b> " + String(currentRecord.ssid_count) + "</p>";
  h += "<p><b>SD card:</b> ";
  h += sdReady ? "<span class='ok'>OK</span>" : "<span class='warn'>ERROR</span>";
  h += "</p></div>";

  // File list
  h += F("<h2>Data Files</h2>");
  File root = SD.open("/");
  String rows;
  bool anyFiles = false;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = String(entry.name());
    if (name.endsWith(".csv") || name.endsWith(".CSV")) {
      anyFiles = true;
      rows += "<tr><td>" + name + "</td><td>" + String(entry.size()) + " B</td>"
              "<td><a href='/download?f=" + name + "' class='btn'>&#8595; Download</a></td></tr>";
    }
    entry.close();
  }
  root.close();

  if (anyFiles) {
    h += "<table><tr><th>File</th><th>Size</th><th>Action</th></tr>" + rows + "</table>";
  } else {
    h += F("<div class='card'>No CSV files on SD card yet.</div>");
  }

  // OTA update
  h += F("<h2>Firmware Update (OTA)</h2><div class='card'>"
         "<p>In Arduino IDE: <b>Sketch &rarr; Export Compiled Binary</b></p>"
         "<p>Upload the <b>.bin</b> file — device reboots automatically after flashing.</p>"
         "<form method='POST' action='/update' enctype='multipart/form-data'>"
         "<input type='file' name='firmware' accept='.bin'><br>"
         "<input type='submit' class='btn' value='&#9652; Upload &amp; Flash'>"
         "</form></div>"
         "</body></html>");
  return h;
}

// ======================================================
// WEB SERVER — request handlers
// ======================================================

void handleRoot() {
  server.send(200, "text/html", buildStatusPage());
}

void handleDownload() {
  if (!server.hasArg("f")) { server.send(400, "text/plain", "Missing ?f= param"); return; }
  String fname = "/" + server.arg("f");
  if (!SD.exists(fname)) { server.send(404, "text/plain", "File not found"); return; }
  File f = SD.open(fname, FILE_READ);
  if (!f) { server.send(500, "text/plain", "Cannot open file"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + server.arg("f") + "\"");
  server.streamFile(f, "text/csv");
  f.close();
}

void handleOtaUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    Update.end(true);
    Serial.printf("OTA complete: %u bytes\n", upload.totalSize);
  }
}

void handleOtaDone() {
  if (Update.hasError()) {
    server.send(500, "text/html",
      "<html><body style='background:#111;color:#f00;font-family:monospace;padding:20px'>"
      "<h2>&#10008; Update FAILED — check serial monitor</h2>"
      "<a href='/' style='color:#0af'>Back</a></body></html>");
  } else {
    server.send(200, "text/html",
      "<html><body style='background:#111;color:#0f0;font-family:monospace;padding:20px'>"
      "<h2>&#10004; Update successful — rebooting...</h2></body></html>");
    delay(1500);
    ESP.restart();
  }
}

void handleStatusJson() {
  char buf[384];
  snprintf(buf, sizeof(buf),
    "{"
      "\"gps_fix\":%s,"
      "\"lat\":%.6f,"
      "\"lon\":%.6f,"
      "\"satellites\":%d,"
      "\"hdop\":%.1f,"
      "\"battery_v\":%.2f,"
      "\"locations_today\":%d,"
      "\"ssids_current\":%d,"
      "\"sd_ok\":%s,"
      "\"mode\":\"webserver\""
    "}",
    gps.location.isValid() ? "true" : "false",
    gps.location.isValid() ? gps.location.lat() : 0.0,
    gps.location.isValid() ? gps.location.lng() : 0.0,
    gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
    gps.hdop.isValid() ? gps.hdop.hdop() : -1.0f,
    readBatteryVoltage(),
    totalLoggedLocations,
    currentRecord.ssid_count,
    sdReady ? "true" : "false"
  );
  server.send(200, "application/json", buf);
}

// ======================================================
// MODE SWITCHING
// ======================================================

void startWebServer() {
  // Flush any in-progress location record before going offline
  if (currentRecord.valid) {
    writeRecordToSD(currentRecord);
    resetRecord(currentRecord);
  }
  WiFi.scanDelete();
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(100);

  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/download",    HTTP_GET,  handleDownload);
  server.on("/status.json", HTTP_GET,  handleStatusJson);
  server.on("/update", HTTP_POST, handleOtaDone, handleOtaUpload);
  server.begin();

  systemMode  = MODE_WEBSERVER;
  apStartTime = millis();
  displayPage = PAGE_SERVER;

  Serial.println("Web server started.");
  Serial.print("AP: " AP_SSID "  IP: ");
  Serial.println(WiFi.softAPIP());
}

void stopWebServer() {
  server.stop();
  WiFi.softAPdisconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  systemMode  = MODE_LOGGING;
  displayPage = PAGE_MAIN;
  lastScan    = 0; // trigger an immediate scan on return
  Serial.println("Web server stopped. Resumed logging.");
}

// ======================================================
// BUTTON — short press cycles OLED page, long press toggles web server
// ======================================================

void handleButton() {
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  if (pressed && !buttonWasPressed) {
    buttonPressTime  = millis();
    buttonWasPressed = true;
    longPressHandled = false;
  } else if (pressed && !longPressHandled) {
    if (millis() - buttonPressTime >= (unsigned long)BUTTON_LONG_PRESS_MS) {
      longPressHandled = true;
      if (systemMode == MODE_LOGGING) startWebServer();
      else                            stopWebServer();
    }
  } else if (!pressed && buttonWasPressed) {
    if (!longPressHandled) {
      displayPage = (DisplayPage)((displayPage + 1) % PAGE_COUNT);
    }
    buttonWasPressed = false;
  }
}

// ======================================================
// OLED DISPLAY — 4 pages, cycled by short button press
// ======================================================

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("SSID Logger & Tracker");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  switch (displayPage) {

    case PAGE_MAIN: {
      char timeBuf[16];
      display.setCursor(0, 14);
      display.print("Time: ");
      if (gpsClockSet && time(nullptr) > 1704067200LL) {
        getLocalTimeOnly(timeBuf, sizeof(timeBuf));
        display.println(timeBuf);
      } else {
        display.println("Waiting GPS...");
      }

      display.setCursor(0, 24);
      display.print("GPS: ");
      display.print(gps.location.isValid() ? "FIX" : "No Fix");
      display.print("  Sats:");
      display.println(gps.satellites.isValid() ? (int)gps.satellites.value() : 0);

      display.setCursor(0, 34);
      display.print("SSIDs:");
      display.print(currentRecord.ssid_count);
      display.print("  Locs:");
      display.println(totalLoggedLocations);

      display.setCursor(0, 44);
      char bv[6];
      dtostrf(readBatteryVoltage(), 4, 2, bv);
      display.print("Bat:"); display.print(bv); display.print("V");
      display.print("  SD:");
      display.println(sdReady ? "OK" : "ERR");

      display.setCursor(0, 54);
      display.println(systemMode == MODE_LOGGING ? "[ LOGGING ]" : "[ WEB SRV ]");
      break;
    }

    case PAGE_GPS: {
      display.setCursor(0, 14);
      if (gps.location.isValid()) {
        char buf[28];
        snprintf(buf, sizeof(buf), "Lat: %.5f", gps.location.lat());
        display.println(buf);
        snprintf(buf, sizeof(buf), "Lon: %.5f", gps.location.lng());
        display.println(buf);
        float h = gps.hdop.isValid() ? gps.hdop.hdop() : -1.0f;
        display.print("HDOP: "); display.print(h, 1);
        display.print(" ("); display.print(hdopQuality(h)); display.println(")");
        display.print("Sats: ");
        display.println(gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
      } else {
        display.println("No GPS Fix");
        display.println("Searching...");
      }
      break;
    }

    case PAGE_WIFI: {
      display.setCursor(0, 14);
      display.print("Cur loc SSIDs: ");
      display.println(currentRecord.ssid_count);
      display.print("Locations today: ");
      display.println(totalLoggedLocations);
      display.print("Scan every:  ");
      display.print(SCAN_PERIOD_MS / 1000UL);
      display.println("s");
      display.print("Move thresh: ");
      display.print((int)LOCATION_THRESHOLD_M);
      display.println("m");
      break;
    }

    case PAGE_SERVER: {
      display.setCursor(0, 14);
      if (systemMode == MODE_WEBSERVER) {
        display.println("AP: " AP_SSID);
        display.println("IP: 192.168.4.1");
        unsigned long elapsed  = (millis() - apStartTime) / 1000UL;
        unsigned long limit    = AP_TIMEOUT_MS / 1000UL;
        unsigned long secLeft  = (elapsed < limit) ? (limit - elapsed) : 0;
        char tBuf[20];
        snprintf(tBuf, sizeof(tBuf), "Auto-off: %lum%02lus", secLeft / 60, secLeft % 60);
        display.println(tBuf);
        display.println("Hold btn to exit");
      } else {
        display.println("Web server: OFF");
        display.println("");
        display.println("Hold btn 3s");
        display.println("to start AP");
      }
      break;
    }

    default: break;
  }

  display.display();
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nSSID Logger v2.0  —  starting up");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  analogReadResolution(12);

  gpsSerial.setRxBufferSize(1024); // prevent overflow during 2-4s WiFi scans
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed — halting");
    while (1);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("SSID Logger v2.0");
  display.println("Initializing...");
  display.display();

  setenv("TZ", "MST7MDT,M3.2.0,M11.1.0", 1); // fallback until GPS fix
  tzset();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, SPI);
  Serial.println(sdReady ? "SD card ready." : "SD card FAILED — logging disabled.");

  resetRecord(currentRecord);
  updateDisplay();
}

// ======================================================
// LOOP
// ======================================================

void loop() {
  // Always keep GPS serial buffer drained
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  updateSystemClockFromGPS();
  handleButton();

  if (systemMode == MODE_WEBSERVER) {
    server.handleClient();
    if (millis() - apStartTime >= AP_TIMEOUT_MS) {
      Serial.println("AP timeout — returning to logging.");
      stopWebServer();
    }
  } else {
    if (millis() - lastScan >= SCAN_PERIOD_MS) {
      lastScan = millis();
      if (gpsReadyForLogging()) scanWiFiAndUpdate();
      else                      Serial.println("GPS not ready, skipping scan.");
    }
  }

  if (millis() - lastDisplay >= DISPLAY_UPDATE_MS) {
    lastDisplay = millis();
    updateDisplay();
  }
}

// ======================================================
// UTILITIES
// ======================================================

bool isLeapYear(int y) {
  return (y % 400 == 0) || (y % 4 == 0 && y % 100 != 0);
}

int daysInMonth(int y, int m) {
  const int d[] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
  return (m == 2 && isLeapYear(y)) ? 29 : d[m];
}

time_t makeEpochUTC(int yr, int mo, int dy, int hr, int mn, int sc) {
  time_t days = 0;
  for (int y = 1970; y < yr; y++) days += isLeapYear(y) ? 366 : 365;
  for (int m = 1;    m < mo; m++) days += daysInMonth(yr, m);
  days += dy - 1;
  return days * 86400L + hr * 3600L + mn * 60L + sc;
}

const char* hdopQuality(float h) {
  if (h <   0)  return "No DOP";
  if (h <= 1.0) return "Excellent";
  if (h <= 2.0) return "Very Good";
  if (h <= 5.0) return "Good";
  if (h <= 10.0)return "Moderate";
  if (h <= 20.0)return "Poor";
  return "Very Poor";
}
