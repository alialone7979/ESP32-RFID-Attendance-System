/*
   ESP32-RFID-Attendance-system.ino

   ESP32 RFID Attendance & Access Control System
   -----------------------------------------------
   A complete embedded attendance system built around ESP32.
   The firmware combines RFID authentication, RTC/NTP time
   synchronization, time-based access control, persistent
   IN/OUT state management, OLED user feedback, and Wi-Fi
   communication with a local PHP/Apache server.

   Hardware:
   - ESP32
   - MFRC522 RFID
   - DS1307 RTC
   - Relay
   - Buzzer
   - 0.96" 128x64 SSD1306 OLED
   - 3.3V microSD module

   Communication:
   - Wi-Fi
   - Apache + PHP
   - CSV
   - SD card local attendance backup
   - OTA firmware updates over Wi-Fi

   Features:
   - Persian/Jalali date
   - Iran local time
   - NTP -> DS1307 synchronization
   - RFID IN / OUT
   - Duration calculation
   - IN state survives ESP32 reset using Preferences
   - Multiple IN / OUT records per day
   - Individual time-based access control
   - Relay + buzzer feedback
   - Live OLED clock and access-status display
   - Local append-only attendance log on microSD

   SD card uses the same SPI bus as the RFID module.
   Both devices have separate chip-select (CS) pins.
*/

#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>


// ==================================================
// CONFIGURATION
// ==================================================

// ---------- Wi-Fi ----------

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";


// ---------- Apache/PHP Server ----------

const String serverUrl =
  "http://10.205.203.150/attendance/attendance.php";


// ---------- OTA ----------

const char* otaHostname = "ESP32-RFID-Attendance";


// ---------- RFID ----------

#define RST_PIN 4
#define SS_PIN  5

MFRC522 mfrc522(SS_PIN, RST_PIN);


// ---------- SPI ----------

#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23


// ---------- RTC ----------

#define I2C_SDA 21
#define I2C_SCL 22

RTC_DS1307 rtc;


// ---------- SD CARD ----------

// The SD card shares the SPI bus with the MFRC522.
// Only the CS pin is different.
#define SD_CS_PIN 13

const char* sdLogFile = "/attendance.csv";

bool sdReady = false;


// ---------- OLED ----------

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

bool oledReady = false;
unsigned long lastOLEDUpdate = 0;
unsigned long oledMessageUntil = 0;
String oledTitle = "";
String oledLine1 = "";
String oledLine2 = "";


// ---------- Relay ----------

const int relayPin = 27;


// ---------- Buzzer ----------

const int buzzerPin = 14;


// ==================================================
// ACCESS CONTROL
// ==================================================

/*
   Individual access time for each person.

   Format:

   { "UID", startHour, startMinute, endHour, endMinute }

   Example:

   { "6363F92C", 7, 30, 16, 30 }

   means:

   Access allowed from 07:30 to 16:30.

   The start and end times are inclusive.
*/


struct AccessRule {

  const char* uid;

  uint8_t startHour;
  uint8_t startMinute;

  uint8_t endHour;
  uint8_t endMinute;
};


// --------------------------------------------------
// Define access times for each person here
// --------------------------------------------------

AccessRule accessRules[] = {

  // UID         Start       End

  { "6363F92C",  5, 30,     18, 30 },   // Mr Ahmadi
  { "9B0AC422",  8, 00,     17, 00 },   // Mr Kordloo
  { "5B1D6C22",  9, 00,     15, 00 },   // Mrs Asadi
  { "8B516A22",  8, 00,     16, 00 },   // Mr Rafi Khayat
  { "0BB16322",  8, 00,     16, 00 },   // Mr Mousavi
  { "9B9C6422",  8, 00,     16, 00 },   // Mrs Mahmoodi
  { "CB9BB722",  8, 00,     16, 00 },   // Mr Okhovat
  { "ABBC6922",  0, 00,     23, 59 }    // Test
};


const int accessRuleCount =
  sizeof(accessRules) / sizeof(accessRules[0]);


// ==================================================
// PREFERENCES
// ==================================================

Preferences preferences;


// Maximum number of people whose IN state can be
// stored simultaneously.

#define MAX_RECORDS 50


// ==================================================
// IN-MEMORY RECORD
// ==================================================

struct Record {

  String uid;

  uint32_t timeInUnix;

  bool active;

};

Record records[MAX_RECORDS];


// ==================================================
// UID -> NAME
// ==================================================

String getNameFromUID(const String &uid) {

  if (uid == "6363F92C")
    return "Ali Ahmadi";

  if (uid == "9B0AC422")
    return "Vahid Kordloo";

  if (uid == "5B1D6C22")
    return "Fateme Asadi";

  if (uid == "8B516A22")
    return "Younes Rafi Khayat";

  if (uid == "0BB16322")
    return "Mohammad Mousavi";

  if (uid == "9B9C6422")
    return "Zahra Mahmoodi";

  if (uid == "CB9BB722")
    return "Reza Okhovat";

  if (uid == "ABBC6922")
    return "Test";

  return "UNKNOWN";
}


// ==================================================
// FIND ACCESS RULE
// ==================================================

int findAccessRule(const String &uid) {

  for (int i = 0; i < accessRuleCount; i++) {

    if (uid == accessRules[i].uid) {

      return i;
    }
  }

  return -1;
}


// ==================================================
// CHECK ACCESS TIME
// ==================================================

bool isAccessAllowed(
  const String &uid,
  const DateTime &now
) {

  int ruleIndex =
    findAccessRule(uid);


  // ------------------------------------------------
  // UID does not have an access rule
  // ------------------------------------------------

  if (ruleIndex == -1) {

    Serial.println(
      "❌ No access rule found for this UID"
    );

    return false;
  }


  AccessRule rule =
    accessRules[ruleIndex];


  int currentMinutes =
    now.hour() * 60 +
    now.minute();


  int startMinutes =
    rule.startHour * 60 +
    rule.startMinute;


  int endMinutes =
    rule.endHour * 60 +
    rule.endMinute;


  // ------------------------------------------------
  // Normal time range
  // Example: 07:30 -> 16:30
  // ------------------------------------------------

  if (startMinutes <= endMinutes) {

    if (
      currentMinutes >= startMinutes &&
      currentMinutes <= endMinutes
    ) {

      return true;
    }

    return false;
  }


  // ------------------------------------------------
  // Overnight time range
  // Example: 22:00 -> 06:00
  // ------------------------------------------------

  if (
    currentMinutes >= startMinutes ||
    currentMinutes <= endMinutes
  ) {

    return true;
  }


  return false;
}


// ==================================================
// PRINT ACCESS RULE
// ==================================================

void printAccessRule(
  const String &uid
) {

  int ruleIndex =
    findAccessRule(uid);


  if (ruleIndex == -1) {

    Serial.println(
      "⏰ Allowed time: NOT DEFINED"
    );

    return;
  }


  AccessRule rule =
    accessRules[ruleIndex];


  char startBuffer[6];
  char endBuffer[6];


  sprintf(
    startBuffer,
    "%02d:%02d",
    rule.startHour,
    rule.startMinute
  );


  sprintf(
    endBuffer,
    "%02d:%02d",
    rule.endHour,
    rule.endMinute
  );


  Serial.println(
    "⏰ Allowed time: " +
    String(startBuffer) +
    " - " +
    String(endBuffer)
  );
}


// ==================================================
// JALALI DATE CONVERSION
// ==================================================

void gregorianToJalali(
  int gy,
  int gm,
  int gd,
  int &jy,
  int &jm,
  int &jd
) {

  int g_days_in_month[] = {
    31, 28, 31, 30, 31, 30,
    31, 31, 30, 31, 30, 31
  };

  int j_days_in_month[] = {
    31, 31, 31, 31, 31, 31,
    30, 30, 30, 30, 30, 29
  };


  int gy2 = gy - 1600;
  int gm2 = gm - 1;
  int gd2 = gd - 1;


  long g_day_no =
    365L * gy2 +
    (gy2 + 3) / 4 -
    (gy2 + 99) / 100 +
    (gy2 + 399) / 400;


  for (int i = 0; i < gm2; i++)
    g_day_no += g_days_in_month[i];


  if (
    gm > 2 &&
    (
      (gy % 4 == 0 && gy % 100 != 0) ||
      (gy % 400 == 0)
    )
  ) {

    g_day_no++;
  }


  g_day_no += gd2;


  long j_day_no =
    g_day_no - 79;


  long j_np =
    j_day_no / 12053;


  j_day_no %= 12053;


  int jyTmp =
    979 +
    33 * j_np +
    4 * (j_day_no / 1461);


  j_day_no %= 1461;


  if (j_day_no >= 366) {

    jyTmp +=
      (j_day_no - 1) / 365;

    j_day_no =
      (j_day_no - 1) % 365;
  }


  int i = 0;


  for (
    ;
    i < 11 &&
    j_day_no >= j_days_in_month[i];
    i++
  ) {

    j_day_no -=
      j_days_in_month[i];
  }


  jy = jyTmp;
  jm = i + 1;
  jd = j_day_no + 1;
}


// ==================================================
// GET JALALI DATE
// ==================================================

String getJalaliDate(
  const DateTime &now
) {

  int jy;
  int jm;
  int jd;


  gregorianToJalali(
    now.year(),
    now.month(),
    now.day(),
    jy,
    jm,
    jd
  );


  char buffer[12];


  sprintf(
    buffer,
    "%04d/%02d/%02d",
    jy,
    jm,
    jd
  );


  return String(buffer);
}


// ==================================================
// GET TIME
// ==================================================

String getTimeString(
  const DateTime &now
) {

  char buffer[10];


  sprintf(
    buffer,
    "%02d:%02d:%02d",
    now.hour(),
    now.minute(),
    now.second()
  );


  return String(buffer);
}


// ==================================================
// FIND OPEN RECORD
// ==================================================

int findOpenRecord(
  const String &uid
) {

  for (int i = 0; i < MAX_RECORDS; i++) {

    if (
      records[i].active &&
      records[i].uid == uid
    ) {

      return i;
    }
  }


  return -1;
}


// ==================================================
// FIND FREE RECORD SLOT
// ==================================================

int findFreeRecordSlot() {

  for (int i = 0; i < MAX_RECORDS; i++) {

    if (!records[i].active) {

      return i;
    }
  }


  return -1;
}


// ==================================================
// PREFERENCES KEY
// ==================================================

String makePreferenceKey(
  const String &uid
) {

  return "in_" + uid;
}


// ==================================================
// SAVE IN STATE
// ==================================================

void saveInState(
  const String &uid,
  uint32_t timeInUnix
) {

  String key =
    makePreferenceKey(uid);


  preferences.putUInt(
    key.c_str(),
    timeInUnix
  );


  Serial.println(
    "💾 IN state saved: " +
    uid
  );
}


// ==================================================
// REMOVE IN STATE
// ==================================================

void removeInState(
  const String &uid
) {

  String key =
    makePreferenceKey(uid);


  preferences.remove(
    key.c_str()
  );


  Serial.println(
    "🗑️ IN state removed: " +
    uid
  );
}


// ==================================================
// LOAD IN STATE
// ==================================================

void loadInStates() {

  Serial.println();
  Serial.println(
    "🔄 Loading saved IN states..."
  );


  // Clear RAM records

  for (int i = 0; i < MAX_RECORDS; i++) {

    records[i].uid = "";
    records[i].timeInUnix = 0;
    records[i].active = false;
  }


  // Known UIDs

  const char* knownUIDs[] = {

    "6363F92C",
    "9B0AC422",
    "5B1D6C22",
    "8B516A22",
    "0BB16322",
    "9B9C6422",
    "CB9BB722",
    "ABBC6922"
  };


  const int knownCount =
    sizeof(knownUIDs) /
    sizeof(knownUIDs[0]);


  int slot = 0;


  for (
    int i = 0;
    i < knownCount;
    i++
  ) {

    String uid =
      String(knownUIDs[i]);


    String key =
      makePreferenceKey(uid);


    uint32_t savedTime =
      preferences.getUInt(
        key.c_str(),
        0
      );


    if (
      savedTime > 0 &&
      slot < MAX_RECORDS
    ) {

      records[slot].uid =
        uid;

      records[slot].timeInUnix =
        savedTime;

      records[slot].active =
        true;


      Serial.println(
        "   ↳ IN found: " +
        uid
      );


      slot++;
    }
  }


  Serial.println(
    "✅ Saved IN states loaded"
  );
}


// ==================================================
// WIFI
// ==================================================

bool connectWiFi() {

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    return true;
  }


  Serial.print(
    "📡 Connecting WiFi"
  );


  WiFi.begin(
    ssid,
    password
  );


  unsigned long start =
    millis();


  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 10000
  ) {

    Serial.print(".");
    delay(400);
  }


  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println();

    Serial.println(
      "🌐 WiFi Connected!"
    );


    Serial.print(
      "IP: "
    );


    Serial.println(
      WiFi.localIP()
    );


    return true;
  }


  Serial.println();

  Serial.println(
    "⚠️ WiFi connection failed"
  );


  return false;
}


// ==================================================
// RTC + NTP
// ==================================================

bool initializeRTC() {

  Wire.begin(
    I2C_SDA,
    I2C_SCL
  );


  if (!rtc.begin()) {

    Serial.println(
      "❌ DS1307 not found"
    );

    return false;
  }


  Serial.println(
    "✅ DS1307 detected"
  );


  // Iran local time
  // UTC +3:30
  // No daylight saving adjustment

  configTime(
    3 * 3600 + 30 * 60,
    0,
    "pool.ntp.org",
    "time.google.com"
  );


  Serial.print(
    "⌛ Waiting for NTP..."
  );


  struct tm ntp_tm;


  if (
    getLocalTime(
      &ntp_tm,
      5000
    )
  ) {

    DateTime ntpDateTime(
      ntp_tm.tm_year + 1900,
      ntp_tm.tm_mon + 1,
      ntp_tm.tm_mday,
      ntp_tm.tm_hour,
      ntp_tm.tm_min,
      ntp_tm.tm_sec
    );


    rtc.adjust(
      ntpDateTime
    );


    Serial.println();

    Serial.println(
      "✅ RTC synchronized from NTP"
    );


    Serial.print(
      "Iran Time: "
    );


    Serial.print(
      ntpDateTime.year()
    );

    Serial.print("-");


    Serial.print(
      ntpDateTime.month()
    );

    Serial.print("-");


    Serial.print(
      ntpDateTime.day()
    );

    Serial.print(" ");


    Serial.print(
      ntpDateTime.hour()
    );

    Serial.print(":");


    Serial.print(
      ntpDateTime.minute()
    );

    Serial.print(":");


    Serial.println(
      ntpDateTime.second()
    );

  } else {

    Serial.println();

    Serial.println(
      "⚠️ NTP failed"
    );

    Serial.println(
      "⚠️ Using RTC stored time"
    );
  }


  return true;
}


// ==================================================
// BUZZER
// ==================================================

void beep() {

  tone(
    buzzerPin,
    100
  );


  delay(500);


  noTone(
    buzzerPin
  );


  delay(300);
}


// ==================================================
// RELAY
// ==================================================

void activateRelay(
  unsigned long duration
) {

  digitalWrite(
    relayPin,
    LOW
  );


  delay(
    duration
  );


  digitalWrite(
    relayPin,
    HIGH
  );
}


// ==================================================
// SEND DATA TO APACHE
// ==================================================

bool postToServer(
  const String &uid,
  const String &name,
  const String &dateJalali,
  const String &timeIn,
  const String &timeOut,
  int duration
) {

  if (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    Serial.println(
      "⚠️ WiFi Offline - "
      "cannot POST"
    );

    return false;
  }


  HTTPClient http;


  http.begin(
    serverUrl
  );


  http.addHeader(
    "Content-Type",
    "application/x-www-form-urlencoded; charset=UTF-8"
  );


  String postData =
    "uid=" + uid +
    "&name=" + name +
    "&date=" + dateJalali +
    "&timeIn=" + timeIn +
    "&timeOut=" + timeOut +
    "&duration=" + String(duration);


  int httpCode =
    http.POST(postData);


  if (httpCode > 0) {

    String response =
      http.getString();


    Serial.println(
      "✅ Server: HTTP " +
      String(httpCode) +
      " -> " +
      response
    );


    http.end();


    return (
      httpCode >= 200 &&
      httpCode < 300
    );
  }


  Serial.println(
    "❌ HTTP Error: " +
    String(httpCode)
  );


  http.end();


  return false;
}


// ==================================================
// GET UID FROM RFID
// ==================================================

String getUID() {

  String uid = "";


  for (
    byte i = 0;
    i < mfrc522.uid.size;
    i++
  ) {

    char buffer[3];


    sprintf(
      buffer,
      "%02X",
      mfrc522.uid.uidByte[i]
    );


    uid += String(buffer);
  }


  return uid;
}


// ==================================================
// SET IN
// ==================================================

bool registerIN(
  const String &uid,
  uint32_t timeInUnix
) {

  int slot =
    findFreeRecordSlot();


  if (slot == -1) {

    Serial.println(
      "❌ No free IN record slot"
    );

    return false;
  }


  records[slot].uid =
    uid;


  records[slot].timeInUnix =
    timeInUnix;


  records[slot].active =
    true;


  saveInState(
    uid,
    timeInUnix
  );


  return true;
}


// ==================================================
// SET OUT
// ==================================================

void registerOUT(
  int index
) {

  String uid =
    records[index].uid;


  records[index].active =
    false;


  records[index].uid =
    "";


  records[index].timeInUnix =
    0;


  removeInState(
    uid
  );
}


// ==================================================
// SD CARD
// ==================================================

bool initializeSDCard() {

  pinMode(
    SD_CS_PIN,
    OUTPUT
  );

  // Keep SD deselected while RFID is being used.
  digitalWrite(
    SD_CS_PIN,
    HIGH
  );

  if (!SD.begin(SD_CS_PIN, SPI)) {

    Serial.println(
      "WARNING: SD card initialization failed"
    );

    return false;
  }

  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {

    Serial.println(
      "WARNING: No SD card detected"
    );

    return false;
  }

  Serial.println(
    "SD card detected"
  );

  Serial.print(
    "SD card size: "
  );

  Serial.print(
    SD.cardSize() / (1024 * 1024)
  );

  Serial.println(
    " MB"
  );

  if (!SD.exists(sdLogFile)) {

    File file = SD.open(
      sdLogFile,
      FILE_WRITE
    );

    if (!file) {

      Serial.println(
        "WARNING: Could not create SD log file"
      );

      return false;
    }

    file.println(
      "UID,Name,Date,TimeIn,TimeOut,DurationMin,Status"
    );

    file.close();
  }

  Serial.println(
    "SD attendance log ready"
  );

  return true;
}


bool logAttendanceToSD(
  const String &uid,
  const String &name,
  const String &dateJalali,
  const String &timeIn,
  const String &timeOut,
  int duration,
  const String &status
) {

  if (!sdReady) {
    return false;
  }

  // Keep RFID deselected while SD is being accessed.
  digitalWrite(
    SS_PIN,
    HIGH
  );

  File file = SD.open(
    sdLogFile,
    FILE_APPEND
  );

  if (!file) {

    Serial.println(
      "ERROR: Could not open SD attendance log"
    );

    return false;
  }

  file.print("\"");
  file.print(uid);
  file.print("\",\"");
  file.print(name);
  file.print("\",\"");
  file.print(dateJalali);
  file.print("\",\"");
  file.print(timeIn);
  file.print("\",\"");
  file.print(timeOut);
  file.print("\",");
  file.print(duration);
  file.print(",\"");
  file.print(status);
  file.println("\"");

  file.close();

  Serial.println(
    "SD: attendance saved"
  );

  return true;
}


// ==================================================
// OLED DISPLAY
// ==================================================

String getEnglishNameFromUID(const String &uid) {

  if (uid == "6363F92C") return "ALI AHMADI";
  if (uid == "9B0AC422") return "VAHID KORDLO";
  if (uid == "5B1D6C22") return "FATEME ASADI";
  if (uid == "8B516A22") return "YOUNES RAFI KHAYAT";
  if (uid == "0BB16322") return "MOHAMMAD MOUSAVI";
  if (uid == "9B9C6422") return "ZAHRA MAHMOUDI";
  if (uid == "CB9BB722") return "REZA OKHOVAT";
  if (uid == "ABBC6922") return "TEST";

  return "UNKNOWN";
}


void drawOLEDClock() {

  if (!oledReady) return;

  DateTime now = rtc.now();

  String date = getJalaliDate(now);
  String time = getTimeString(now);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("RFID ATTENDANCE");

  display.setTextSize(2);
  display.setCursor(14, 14);
  display.println(time);

  display.setTextSize(1);
  display.setCursor(18, 38);
  display.print("Date: ");
  display.println(date);

  display.setCursor(17, 52);
  display.println("Scan your card");

  display.display();
}


void showOLEDStatus(
  const String &title,
  const String &line1,
  const String &line2,
  unsigned long durationMs = 2500
) {

  if (!oledReady) return;

  oledTitle = title;
  oledLine1 = line1;
  oledLine2 = line2;
  oledMessageUntil = millis() + durationMs;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(oledTitle);

  display.setCursor(0, 20);
  display.println(oledLine1);

  display.setCursor(0, 38);
  display.println(oledLine2);

  DateTime now = rtc.now();
  display.setCursor(0, 55);
  display.println(getTimeString(now));

  display.display();
}


void updateOLED() {

  if (!oledReady) return;

  unsigned long nowMs = millis();

  if (nowMs < oledMessageUntil) {
    return;
  }

  if (nowMs - lastOLEDUpdate >= 1000) {
    lastOLEDUpdate = nowMs;
    drawOLEDClock();
  }
}


// ==================================================
// OTA
// ==================================================

void initializeOTA() {

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("⚠️ OTA skipped: Wi-Fi not connected");

    return;
  }


  ArduinoOTA.setHostname(otaHostname);


  ArduinoOTA.onStart([]() {

    String type = (ArduinoOTA.getCommand() == U_FLASH)
                  ? "firmware"
                  : "filesystem";

    Serial.println("🔄 OTA update started: " + type);
  });


  ArduinoOTA.onEnd([]() {

    Serial.println("\n✅ OTA update finished");
  });


  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {

    Serial.printf("OTA Progress: %u%%\r", (progress * 100) / total);
  });


  ArduinoOTA.onError([](ota_error_t error) {

    Serial.printf("\n❌ OTA Error [%u]: ", error);

    if (error == OTA_AUTH_ERROR) {
      Serial.println("Authentication Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    } else {
      Serial.println("Unknown Error");
    }
  });


  ArduinoOTA.begin();


  Serial.println("✅ OTA Ready");
  Serial.print("OTA Hostname: ");
  Serial.println(otaHostname);
  Serial.print("OTA IP: ");
  Serial.println(WiFi.localIP());
}


// ==================================================
// SETUP
// ==================================================

void setup() {

  Serial.begin(
    115200
  );


  delay(500);


  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "   ESP32 RFID ATTENDANCE"
  );

  Serial.println(
    "================================"
  );


  // ------------------------------------------------
  // GPIO
  // ------------------------------------------------

  pinMode(
    relayPin,
    OUTPUT
  );


  // Relay inactive

  digitalWrite(
    relayPin,
    HIGH
  );


  pinMode(
    buzzerPin,
    OUTPUT
  );


  digitalWrite(
    buzzerPin,
    LOW
  );


  // ------------------------------------------------
  // Preferences
  // ------------------------------------------------

  preferences.begin(
    "attendance",
    false
  );


  // ------------------------------------------------
  // SPI + RFID
  // ------------------------------------------------

  SPI.begin(
    SPI_SCK,
    SPI_MISO,
    SPI_MOSI
  );

  // Keep SD deselected while RFID is initialized.
  pinMode(
    SD_CS_PIN,
    OUTPUT
  );

  digitalWrite(
    SD_CS_PIN,
    HIGH
  );

  pinMode(
    SS_PIN,
    OUTPUT
  );

  digitalWrite(
    SS_PIN,
    HIGH
  );

  mfrc522.PCD_Init();


  delay(50);


  Serial.println(
    "✅ RFID Ready"
  );


  // ------------------------------------------------
  // SD card
  // ------------------------------------------------

  sdReady = initializeSDCard();


  // ------------------------------------------------
  // Wi-Fi
  // ------------------------------------------------

  connectWiFi();


  // ------------------------------------------------
  // RTC
  // ------------------------------------------------

  initializeRTC();


  // ------------------------------------------------
  // OLED
  // ------------------------------------------------

  oledReady = display.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_ADDRESS
  );

  if (oledReady) {

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("RFID ATTENDANCE");
    display.setCursor(0, 20);
    display.println("Initializing...");
    display.display();
  } else {

    Serial.println("WARNING: OLED not found");
  }


  // ------------------------------------------------
  // OTA
  // ------------------------------------------------

  initializeOTA();


  // ------------------------------------------------
  // Load saved IN states
  // ------------------------------------------------

  loadInStates();

  if (oledReady) {
    drawOLEDClock();
  }


  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "🚀 System Ready"
  );

  Serial.println(
    "================================"
  );
}


// ==================================================
// MAIN LOOP
// ==================================================

void loop() {

  // ------------------------------------------------
  // Handle OTA requests
  // ------------------------------------------------

  ArduinoOTA.handle();


  // ------------------------------------------------
  // Keep OLED clock updated
  // ------------------------------------------------

  updateOLED();


  // ------------------------------------------------
  // Make sure RFID is ready
  // ------------------------------------------------

  if (
    !mfrc522.PICC_IsNewCardPresent()
  ) {

    delay(50);

    return;
  }


  if (
    !mfrc522.PICC_ReadCardSerial()
  ) {

    delay(50);

    return;
  }


  // ------------------------------------------------
  // Get UID
  // ------------------------------------------------

  String uid =
    getUID();


  String name =
    getNameFromUID(uid);

  String displayName =
    getEnglishNameFromUID(uid);

  showOLEDStatus(
    "CARD DETECTED",
    displayName,
    "Checking access..."
  );


  // ------------------------------------------------
  // Current time
  // ------------------------------------------------

  DateTime now =
    rtc.now();


  String dateJalali =
    getJalaliDate(now);


  String currentTime =
    getTimeString(now);


  Serial.println();

  Serial.println(
    "--------------------------------"
  );


  Serial.println(
    "🎫 UID: " +
    uid
  );


  Serial.println(
    "👤 Name: " +
    name
  );


  Serial.println(
    "📅 Date: " +
    dateJalali
  );


  Serial.println(
    "⏰ Time: " +
    currentTime
  );


  // =================================================
  // ACCESS CONTROL
  // =================================================

  printAccessRule(uid);


  bool accessAllowed =
    isAccessAllowed(
      uid,
      now
    );


  // ------------------------------------------------
  // Access denied
  // ------------------------------------------------

  if (!accessAllowed) {

    Serial.println(
      "🚫 ACCESS DENIED"
    );


    Serial.println(
      "🔒 Relay remains OFF"
    );


    Serial.println(
      "⚠️ Attendance NOT registered"
    );


    // Stop RFID communication

    mfrc522.PICC_HaltA();

    mfrc522.PCD_StopCrypto1();


    showOLEDStatus(
      "ACCESS DENIED",
      displayName,
      "Try again later",
      1500
    );

    // Short delay before another scan

    delay(1500);


    Serial.println(
      "--------------------------------"
    );


    return;
  }


  // ------------------------------------------------
  // Access allowed
  // ------------------------------------------------

  Serial.println(
    "✅ ACCESS ALLOWED"
  );


  // ------------------------------------------------
  // Find previous IN
  // ------------------------------------------------

  int index =
    findOpenRecord(uid);


  String timeInStr =
    currentTime;


  String timeOutStr =
    "";


  int durationMin =
    0;


  // =================================================
  // OUT
  // =================================================

  if (index != -1) {

    Serial.println(
      "🔴 Status: OUT"
    );

    showOLEDStatus(
      "ACCESS GRANTED",
      displayName,
      "CHECK OUT"
    );


    DateTime timeIn(
      records[index].timeInUnix
    );


    timeInStr =
      getTimeString(timeIn);


    timeOutStr =
      currentTime;


    uint32_t elapsed =
      now.unixtime() -
      records[index].timeInUnix;


    durationMin =
      elapsed / 60;


    Serial.print(
      "🕐 Time IN: "
    );


    Serial.println(
      timeInStr
    );


    Serial.print(
      "🕐 Time OUT: "
    );


    Serial.println(
      timeOutStr
    );


    Serial.print(
      "⏱ Duration: "
    );


    Serial.print(
      durationMin
    );


    Serial.println(
      " min"
    );


    // ------------------------------------------------
    // Remove saved IN
    // ------------------------------------------------

    registerOUT(index);


    // ------------------------------------------------
    // Feedback
    // ------------------------------------------------

    beep();


    activateRelay(
      1200
    );


  // =================================================
  // IN
  // =================================================

  } else {

    Serial.println(
      "🟢 Status: IN"
    );

    showOLEDStatus(
      "ACCESS GRANTED",
      displayName,
      "CHECK IN"
    );


    if (
      registerIN(
        uid,
        now.unixtime()
      )
    ) {

      Serial.println(
        "🕐 Time IN: " +
        currentTime
      );


      beep();


      activateRelay(
        800
      );

    } else {

      Serial.println(
        "❌ Could not save IN state"
      );
    }
  }


  // ------------------------------------------------
  // Save attendance to SD card first
  // ------------------------------------------------

  // SD is the local backup, so save the attendance record
  // before attempting the network request.

  String attendanceStatus =
    (index != -1) ? "OUT" : "IN";

  bool sdOK =
    logAttendanceToSD(
      uid,
      name,
      dateJalali,
      timeInStr,
      timeOutStr,
      durationMin,
      attendanceStatus
    );

  if (!sdOK) {

    Serial.println(
      "⚠️ SD log failed"
    );
  }


  // ------------------------------------------------
  // Send to Apache/PHP
  // ------------------------------------------------

  Serial.println(
    "🌐 Sending attendance to server..."
  );


  bool serverOK =
    postToServer(
      uid,
      name,
      dateJalali,
      timeInStr,
      timeOutStr,
      durationMin
    );


  if (!serverOK) {

    Serial.println(
      "⚠️ Server update failed"
    );
  }


  // ------------------------------------------------
  // Stop RFID communication
  // ------------------------------------------------

  mfrc522.PICC_HaltA();

  mfrc522.PCD_StopCrypto1();


  // ------------------------------------------------
  // Prevent immediate re-scan
  // ------------------------------------------------

  delay(1500);


  Serial.println(
    "--------------------------------"
  );
}
