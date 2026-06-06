#include <TinyGPSPlus.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <sys/time.h>

TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

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

// Timezone (Mountain Time with DST)
#define LOCAL_TIMEZONE "MST7MDT,M3.2.0/2,M11.1.0/2"

// Display / logging refresh period
#define SCAN_PERIOD_SEC 10
#define SCAN_PERIOD_MS (SCAN_PERIOD_SEC * 1000UL)

// ======================================================

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

unsigned long lastReport = 0;
unsigned long lastClockSet = 0;

bool gpsClockSet = false;

// ======================================================
// SETUP
// ======================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  // GPS UART
  gpsSerial.begin(
    9600,
    SERIAL_8N1,
    GPS_RX,
    GPS_TX
  );

  // OLED I2C
  Wire.begin(OLED_SDA, OLED_SCL);

  // OLED init
  if (!display.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDR
      )) {

    Serial.println("OLED allocation failed");

    while (1);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();

  // Timezone
  setenv("TZ", LOCAL_TIMEZONE, 1);
  tzset();

  Serial.println();
  Serial.println("SSID Logger & Tracker");
  Serial.print("Scan period: ");
  Serial.print(SCAN_PERIOD_SEC);
  Serial.println(" sec");
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop() {

  // IMPORTANT:
  // Always continuously read GPS stream
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  // Sync clock from GPS
  updateSystemClockFromGPS();

  // Refresh display/report periodically
  if (millis() - lastReport >= SCAN_PERIOD_MS) {

    lastReport = millis();

    updateDisplay();
  }
}

// ======================================================
// GPS CLOCK SYNC
// ======================================================

void updateSystemClockFromGPS() {

  if (!gps.time.isValid()) return;
  if (!gps.date.isValid()) return;

  // Reject bogus GPS default dates
  if (gps.date.year() < 2024) return;

  // Avoid too-frequent updates
  if (gpsClockSet &&
      millis() - lastClockSet < 10000) {
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
  now.tv_usec =
    gps.time.centisecond() * 10000;

  settimeofday(&now, nullptr);

  gpsClockSet = true;
  lastClockSet = millis();

  Serial.println(
    "System clock synchronized from GPS UTC time."
  );
}

// ======================================================
// OLED DISPLAY
// ======================================================

void updateDisplay() {

  display.clearDisplay();

  // --------------------------------
  // Header
  // --------------------------------

  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("SSID Logger & Tracker");

  display.drawLine(
    0,
    10,
    127,
    10,
    SSD1306_WHITE
  );

  // --------------------------------
  // Time
  // --------------------------------

  display.setCursor(0, 16);

  display.print("Time: ");

  time_t now = time(nullptr);

  if (gpsClockSet &&
      now > 1704067200) {

    struct tm localTime;

    localtime_r(
      &now,
      &localTime
    );

    char timeBuffer[16];

    strftime(
      timeBuffer,
      sizeof(timeBuffer),
      "%H:%M:%S",
      &localTime
    );

    display.println(timeBuffer);

    Serial.print("Local time: ");
    Serial.println(timeBuffer);

  } else {

    display.println("Waiting...");

    Serial.println(
      "GPS time not available yet."
    );
  }

  // --------------------------------
  // GPS Fix
  // --------------------------------

  display.setCursor(0, 30);

  display.print("GPS: ");

  if (gps.location.isValid()) {
    display.println("FIX");
  } else {
    display.println("No Fix");
  }

  // --------------------------------
  // Satellites + Quality
  // --------------------------------

  display.setCursor(0, 42);

  display.print("Sat: ");

  if (gps.satellites.isValid()) {
    display.print(
      gps.satellites.value()
    );
  } else {
    display.print("--");
  }

  display.print("  ");

  if (gps.hdop.isValid()) {

    float hdop = gps.hdop.hdop();

    if (hdop <= 1.0) {
      display.print("Excellent");
    }
    else if (hdop <= 2.0) {
      display.print("Very Good");
    }
    else if (hdop <= 5.0) {
      display.print("Good");
    }
    else if (hdop <= 10.0) {
      display.print("Moderate");
    }
    else if (hdop <= 20.0) {
      display.print("Poor");
    }
    else {
      display.print("Very Poor");
    }

  } else {

    display.print("No DOP");
  }

  // --------------------------------
  // Location
  // --------------------------------

  display.setCursor(0, 54);

  if (gps.location.isValid()) {

    double lon =
      gps.location.lng();

    double lat =
      gps.location.lat();

    char locBuffer[32];

    // 4 decimals fits OLED width
    snprintf(
      locBuffer,
      sizeof(locBuffer),
      "Loc:[%.4f,%.4f]",
      lon,
      lat
    );

    display.print(locBuffer);

  } else {

    display.print(
      "Loc:[Waiting...]"
    );
  }

  display.display();

  // --------------------------------
  // Serial debug output
  // --------------------------------

  Serial.println();

  if (gps.location.isValid()) {

    Serial.print("Location: ");

    Serial.print(
      gps.location.lat(),
      6
    );

    Serial.print(", ");

    Serial.println(
      gps.location.lng(),
      6
    );
  }

  Serial.print("Satellites: ");

  if (gps.satellites.isValid()) {
    Serial.println(
      gps.satellites.value()
    );
  } else {
    Serial.println("--");
  }
}

// ======================================================
// TIME UTILITIES
// ======================================================

bool isLeapYear(int year) {

  if (year % 400 == 0)
    return true;

  if (year % 100 == 0)
    return false;

  return year % 4 == 0;
}

int daysInMonth(
  int year,
  int month
) {

  switch (month) {

    case 1: return 31;

    case 2:
      return isLeapYear(year)
             ? 29 : 28;

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

    default:
      return 30;
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

  for (int y = 1970;
       y < year;
       y++) {

    days +=
      isLeapYear(y)
      ? 366 : 365;
  }

  for (int m = 1;
       m < month;
       m++) {

    days +=
      daysInMonth(year, m);
  }

  days += day - 1;

  return
    days * 86400L +
    hour * 3600L +
    minute * 60L +
    second;
}