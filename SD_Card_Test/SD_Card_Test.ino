#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <sys/time.h>

// ======================================================
// CONFIGURATION
// ======================================================

// GPS UART
#define GPS_RX A0
#define GPS_TX A1

// OLED I2C
#define OLED_SDA A4
#define OLED_SCL A5

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3D

// SD card SPI pins
#define SD_CS   D4
#define SD_MOSI D11
#define SD_MISO D12
#define SD_SCK  D13

// Mountain Time with DST
#define LOCAL_TIMEZONE "MST7MDT,M3.2.0/2,M11.1.0/2"

// WiFi scan/log period
#define SCAN_PERIOD_SEC 30
#define SCAN_PERIOD_MS (SCAN_PERIOD_SEC * 1000UL)

// Clock sync period
#define CLOCK_SYNC_PERIOD_MS 10000UL

// ======================================================

TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

unsigned long lastScan = 0;
unsigned long lastDisplay = 0;
unsigned long lastClockSet = 0;

bool gpsClockSet = false;
bool sdReady = false;

int lastWifiCount = -1;
int totalLoggedRows = 0;

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("SSID Logger & Tracker");

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED allocation failed");
    while (1);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("SSID Logger");
  display.println("Starting...");
  display.display();

  setenv("TZ", LOCAL_TIMEZONE, 1);
  tzset();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (SD.begin(SD_CS, SPI)) {
    sdReady = true;
    Serial.println("SD card ready.");
  } else {
    sdReady = false;
    Serial.println("SD card failed.");
  }

  updateDisplay();
}

// ======================================================
// LOOP
// ======================================================

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  updateSystemClockFromGPS();

  if (millis() - lastDisplay >= 1000) {
    lastDisplay = millis();
    updateDisplay();
  }

  if (millis() - lastScan >= SCAN_PERIOD_MS) {
    lastScan = millis();

    if (gpsReadyForLogging()) {
      scanWiFiAndLog();
    } else {
      Serial.println("Skipping WiFi scan/log: GPS not ready.");
    }
  }
}

// ======================================================
// GPS CLOCK
// ======================================================

void updateSystemClockFromGPS() {
  if (!gps.time.isValid()) return;
  if (!gps.date.isValid()) return;
  if (gps.date.year() < 2024) return;

  if (gpsClockSet && millis() - lastClockSet < CLOCK_SYNC_PERIOD_MS) {
    return;
  }

  time_t epochUTC = makeEpochUTC(
    gps.date.year(),
    gps.date.month(),
    gps.date.day(),
    gps.time.hour(),
    gps.time.minute(),
    gps.time.second()
  );

  struct timeval now;
  now.tv_sec = epochUTC;
  now.tv_usec = gps.time.centisecond() * 10000;

  settimeofday(&now, nullptr);

  gpsClockSet = true;
  lastClockSet = millis();

  Serial.println("System clock synchronized from GPS.");
}

bool gpsReadyForLogging() {
  return gpsClockSet &&
         gps.location.isValid() &&
         gps.date.isValid() &&
         gps.time.isValid() &&
         gps.date.year() >= 2024;
}

// ======================================================
// WIFI SCAN + SD LOGGING
// ======================================================

void scanWiFiAndLog() {
  if (!sdReady) {
    Serial.println("Skipping log: SD not ready.");
    return;
  }

  Serial.println();
  Serial.println("WiFi scan start...");

  int n = WiFi.scanNetworks(false, true);

  lastWifiCount = n;

  Serial.print("WiFi scan done. Networks found: ");
  Serial.println(n);

  if (n <= 0) {
    WiFi.scanDelete();
    return;
  }

  String filename = getDailyFilename();

  ensureCsvHeader(filename);

  File file = SD.open(filename, FILE_APPEND);

  if (!file) {
    Serial.print("Failed to open file: ");
    Serial.println(filename);
    WiFi.scanDelete();
    return;
  }

  char localTime[24];
  char utcTime[24];

  getLocalDateTime(localTime, sizeof(localTime));
  getUtcDateTime(utcTime, sizeof(utcTime));

  double lat = gps.location.lat();
  double lon = gps.location.lng();

  int sats = gps.satellites.isValid() ? gps.satellites.value() : -1;
  float hdop = gps.hdop.isValid() ? gps.hdop.hdop() : -1.0;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    String bssid = WiFi.BSSIDstr(i);
    int rssi = WiFi.RSSI(i);
    int channel = WiFi.channel(i);
    String enc = encryptionToString(WiFi.encryptionType(i));

    file.print(localTime);
    file.print(",");

    file.print(utcTime);
    file.print(",");

    file.print(lon, 6);
    file.print(",");

    file.print(lat, 6);
    file.print(",");

    file.print(sats);
    file.print(",");

    file.print(hdop, 2);
    file.print(",");

    file.print(hdopQuality(hdop));
    file.print(",");

    file.print(channel);
    file.print(",");

    file.print(rssi);
    file.print(",");

    file.print(enc);
    file.print(",");

    file.print(bssid);
    file.print(",");

    printCsvString(file, ssid);

    file.println();

    totalLoggedRows++;
  }

  file.close();
  WiFi.scanDelete();

  Serial.print("Logged to: ");
  Serial.println(filename);
}

// ======================================================
// OLED DISPLAY
// ======================================================

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("SSID Logger & Tracker");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 16);
  display.print("Time: ");

  if (gpsClockSet && time(nullptr) > 1704067200) {
    char timeBuffer[16];
    getLocalTimeOnly(timeBuffer, sizeof(timeBuffer));
    display.println(timeBuffer);
  } else {
    display.println("Waiting...");
  }

  display.setCursor(0, 28);
  display.print("GPS: ");

  if (gps.location.isValid()) {
    display.print("FIX");
  } else {
    display.print("No Fix");
  }

  display.print(" Sat:");

  if (gps.satellites.isValid()) {
    display.println(gps.satellites.value());
  } else {
    display.println("--");
  }

  display.setCursor(0, 40);
  display.print("WiFi: ");

  if (lastWifiCount >= 0) {
    display.print(lastWifiCount);
    display.print(" nets");
  } else {
    display.print("waiting");
  }

  display.print(" SD:");

  if (sdReady) {
    display.println("OK");
  } else {
    display.println("ERR");
  }

  display.setCursor(0, 52);

  if (gps.location.isValid()) {
    char locBuffer[32];

    snprintf(
      locBuffer,
      sizeof(locBuffer),
      "Loc:[%.4f,%.4f]",
      gps.location.lng(),
      gps.location.lat()
    );

    display.print(locBuffer);
  } else {
    display.print("Loc:[Waiting...]");
  }

  display.display();
}

// ======================================================
// FILE + TIME HELPERS
// ======================================================

String getDailyFilename() {
  time_t now = time(nullptr);
  struct tm localTime;
  localtime_r(&now, &localTime);

  char filename[24];

  snprintf(
    filename,
    sizeof(filename),
    "/%04d%02d%02d.csv",
    localTime.tm_year + 1900,
    localTime.tm_mon + 1,
    localTime.tm_mday
  );

  return String(filename);
}

void ensureCsvHeader(String filename) {
  if (SD.exists(filename)) return;

  File file = SD.open(filename, FILE_WRITE);

  if (!file) return;

  file.println(
    "local_time,utc_time,lon,lat,satellites,hdop,quality,channel,rssi,encryption,bssid,ssid"
  );

  file.close();
}

void getLocalDateTime(char *buffer, size_t bufferSize) {
  time_t now = time(nullptr);
  struct tm localTime;
  localtime_r(&now, &localTime);

  strftime(buffer, bufferSize, "%Y-%m-%d %H:%M:%S", &localTime);
}

void getLocalTimeOnly(char *buffer, size_t bufferSize) {
  time_t now = time(nullptr);
  struct tm localTime;
  localtime_r(&now, &localTime);

  strftime(buffer, bufferSize, "%H:%M:%S", &localTime);
}

void getUtcDateTime(char *buffer, size_t bufferSize) {
  time_t now = time(nullptr);
  struct tm utcTime;
  gmtime_r(&now, &utcTime);

  strftime(buffer, bufferSize, "%Y-%m-%d %H:%M:%S", &utcTime);
}

// ======================================================
// WIFI HELPERS
// ======================================================

String encryptionToString(wifi_auth_mode_t type) {
  switch (type) {
    case WIFI_AUTH_OPEN:
      return "open";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA+WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2+WPA3";
    default:
      return "unknown";
  }
}

const char* hdopQuality(float hdop) {
  if (hdop < 0) return "No DOP";
  if (hdop <= 1.0) return "Excellent";
  if (hdop <= 2.0) return "Very Good";
  if (hdop <= 5.0) return "Good";
  if (hdop <= 10.0) return "Moderate";
  if (hdop <= 20.0) return "Poor";
  return "Very Poor";
}

void printCsvString(File &file, String value) {
  file.print("\"");

  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value.charAt(i);

    if (c == '"') {
      file.print("\"\"");
    } else {
      file.print(c);
    }
  }

  file.print("\"");
}

// ======================================================
// UTC EPOCH HELPERS
// ======================================================

bool isLeapYear(int year) {
  if (year % 400 == 0) return true;
  if (year % 100 == 0) return false;
  return year % 4 == 0;
}

int daysInMonth(int year, int month) {
  switch (month) {
    case 1: return 31;
    case 2: return isLeapYear(year) ? 29 : 28;
    case 3: return 31;
    case 4: return 30;
    case 5: return 31;
    case 6: return 30;
    case 7: return 31;
    case 8: return 31;
    case 9: return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
    default: return 30;
  }
}

time_t makeEpochUTC(
  int year,
  int month,
  int day,
  int hour,
  int minute,
  int second
) {
  time_t days = 0;

  for (int y = 1970; y < year; y++) {
    days += isLeapYear(y) ? 366 : 365;
  }

  for (int m = 1; m < month; m++) {
    days += daysInMonth(year, m);
  }

  days += day - 1;

  return days * 86400L +
         hour * 3600L +
         minute * 60L +
         second;
}
