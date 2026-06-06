#include <SPI.h>
#include <FS.h>
#include "SPIFFS.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include "time.h"
#include "config/credits.h"
#include "config/robotbold10.h"
#include "config/seg.h"
#include "config/icons.h"

// NTP Server Configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;      // GMT+1 for Nigeria (West Africa Time)
const int daylightOffset_sec = 0;     // No daylight saving in Nigeria

// DHT11 Configuration
#define DHTPIN 4           // DHT11 Data pin (GPIO 4)
#define DHTTYPE DHT11      // DHT11 sensor type

// L298N Motor Driver Pins (matching working test code)
#define ENB 14             // PWM Speed Control (GPIO 14) - was enable1Pin
#define IN3 27             // Motor Direction Pin 1 (GPIO 27) - was motor1Pin1
#define IN4 26             // Motor Direction Pin 2 (GPIO 26) - was motor1Pin2

// Temperature Control Settings
const float tempMin = 20.0;    // Temperature (°C) where fan starts
const float tempMax = 25.0;    // Temperature (°C) where fan hits 100%

// PWM Configuration for Motor Speed Control (matching working test)
const int pwmFreq = 30000;     // 30 KHz frequency
const int pwmChannel = 0;      // PWM channel 0
const int pwmResolution = 8;   // 8-bit resolution (0-255)

RTC_DS1307 rtc;

DHT dht(DHTPIN, DHTTYPE);

char nameoftheday[7][12] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
char month_name[12][12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
int day_, month_, year_, hour24_, hour12_, minute_, second_, dtw_;
uint8_t id;
WebServer server(80);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define Buzzer 12

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// JSON file paths
const char* employeesPath = "/employees.json";
const char* attendancePath = "/attendance.json";
const char* temperatureRecordsPath = "/temperature_records.json";

String mdnsdotlocalurl = "";
String ssid_, pass_, ip_, gateway_, dispname_, wwwid_, wwwpass_, mdns_, dhcpcheck;
String matricNo, Empname, EmpEmail, EmpPos, Empfid;
bool booting = false;
bool rtcSynced = false;

const char* ssidPath = "/ssid.txt";
const char* passPath = "/pass.txt";
const char* ipPath = "/ip.txt";
const char* gatewayPath = "/gateway.txt";
const char* dispnamePath = "/dispname.txt";
const char* wwwidPath = "/wwwid.txt";
const char* wwwpassPath = "/wwwpass.txt";
const char* mdnsPath = "/mdns.txt";
const char* dhcpcheckPath = "/dhcpcheck.txt";

bool apmode = false;
IPAddress localIP(0, 0, 0, 0);
IPAddress gatewayIP(0, 0, 0, 0);
IPAddress subnetMask(255, 255, 255, 0);
uint8_t max_connections = 8;
unsigned long lastwificheck = 0;
unsigned long lastNtpSync = 0;
unsigned long lastTempCheck = 0;
unsigned long lastDisplayUpdate = 0;
String web_content = "";

// Global temperature and fan speed
float currentTemp = 0.0;
int currentFanSpeed = 0;
String fanStatus = "OFF";

// Function declarations
void oledDisplayCenter(String text, int x, int y);
String readFile(fs::FS &fs, const char *path);
void writeFile(fs::FS &fs, const char *path, const char *message);
void appendFile(fs::FS &fs, const char *path, const char *message);
bool loadFromSPIFFS(String path);
bool is_authentified();
void handleLogin();
void logout();
void handleRoot();
void Settings();
void handleNotFound();
void insertRecord();
void save();
void getssid();
void getmdns();
void getip();
void getfpid();
int FingerprintID();
uint8_t deleteFingerprint(uint8_t id);
uint8_t getFingerprintEnroll();
void connectwifi();
void syncRTCWithNTP();
void controlFanSpeed();

// Buzzer functions
void beepSuccess();
void beepFailure();
void beepError();
void beepStop();

// JSON storage functions
bool saveEmployee(int fpid, String matricNo, String name, String email, String position);
bool getEmployee(int fpid, String &matricNo, String &name, String &email, String &position);
bool deleteEmployee(int fpid);
bool saveAttendance(int fpid, String date, String time, String matricNo, String name, String email, String position, String punchType);
int countTodayPunches(int fpid, String date);
void listAllEmployees();
void listAttendance();
void deleteAttendanceRecord();

// ==================== BUZZER FUNCTIONS ====================

void beepSuccess() {
  tone(Buzzer, 2000, 200);
  delay(200);
  noTone(Buzzer);
}

void beepFailure() {
  for(int i = 0; i < 3; i++) {
    tone(Buzzer, 1500, 100);
    delay(100);
    noTone(Buzzer);
    delay(50);
  }
}

void beepError() {
  tone(Buzzer, 1000, 150);
  delay(150);
  noTone(Buzzer);
  delay(100);
}

void beepStop() {
  noTone(Buzzer);
  digitalWrite(Buzzer, LOW);
}

// ==================== FAN CONTROL FUNCTION ====================

void controlFanSpeed() {
  // Check temperature every 2 seconds
  if (millis() - lastTempCheck < 2000) {
    return;
  }
  lastTempCheck = millis();
  
  // Read temperature from DHT11
  float t = dht.readTemperature();
  
  // Safety check: if sensor fails, turn off fan
if (isnan(t)) {
  Serial.println("⚠ DHT11 Read Error - Fan stopped for safety");
  ledcWrite(ENB, 0);
  currentTemp = 0.0;
    currentFanSpeed = 0;
    fanStatus = "SENSOR ERROR";
    return;
  }
  
  currentTemp = t;
  
  // Calculate fan speed based on temperature
  int fanSpeed = 0;
  
  if (t < tempMin) {
    // Too cold -> Fan OFF
    fanSpeed = 0;
    fanStatus = "OFF";
  } 
  else if (t >= tempMax) {
    // Too hot -> Fan MAX (255)
    fanSpeed = 255;
    fanStatus = "MAX";
  } 
  else {
    // In between -> Scale speed linearly (100 to 255)
    fanSpeed = map((int)(t * 10), (int)(tempMin * 10), (int)(tempMax * 10), 100, 255);
    fanSpeed = constrain(fanSpeed, 100, 255);
    fanStatus = "RUNNING";
  }
  
currentFanSpeed = fanSpeed;

// Apply fan speed using LEDC PWM (matching working code)
ledcWrite(ENB, fanSpeed);

// Debug output

  Serial.print("🌡 Temp: ");
  Serial.print(t, 2);
  Serial.print("°C | Fan Speed: ");
  Serial.print(fanSpeed);
  Serial.print("/255 | Status: ");
  Serial.println(fanStatus);
}

// ==================== NTP SYNC FUNCTION ====================

void syncRTCWithNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi not connected. Cannot sync RTC.");
    return;
  }
  
  Serial.println("🕐 Syncing RTC with NTP server...");
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Syncing Time...");
  display.println("from Internet");
  display.display();
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 10) {
    Serial.println("⏳ Waiting for NTP time...");
    delay(500);
    retries++;
  }
  
  if (retries < 10) {
    rtc.adjust(DateTime(timeinfo.tm_year + 1900, 
                       timeinfo.tm_mon + 1, 
                       timeinfo.tm_mday,
                       timeinfo.tm_hour, 
                       timeinfo.tm_min, 
                       timeinfo.tm_sec));
    
    rtcSynced = true;
    Serial.println("✓ RTC synced with NTP server!");
    Serial.printf("📅 Date/Time: %02d/%02d/%04d %02d:%02d:%02d\n", 
                  timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Time Synced!");
    display.println("");
    display.print("Date: ");
    display.print(timeinfo.tm_mday);
    display.print("/");
    display.print(timeinfo.tm_mon + 1);
    display.print("/");
    display.println(timeinfo.tm_year + 1900);
    display.print("Time: ");
    display.print(timeinfo.tm_hour);
    display.print(":");
    display.print(timeinfo.tm_min);
    display.print(":");
    display.println(timeinfo.tm_sec);
    display.display();
    delay(2000);
    
    lastNtpSync = millis();
  } else {
    Serial.println("❌ Failed to sync with NTP server");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("NTP Sync Failed!");
    display.println("Using RTC time");
    display.display();
    delay(2000);
  }
}

// ==================== SETUP ====================

void setup() {
  booting = true;
  
  // Initialize pins
  pinMode(Buzzer, OUTPUT);
  digitalWrite(Buzzer, LOW);
  pinMode(ENB, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  
  // Configure PWM for motor speed control (using working method)
  ledcAttachChannel(ENB, pwmFreq, pwmResolution, pwmChannel);

  // Set motor direction for continuous forward rotation
  digitalWrite(IN3, LOW);   // Note: LOW/HIGH based on working code
  digitalWrite(IN4, HIGH);  // This makes motor spin forward
  
  Serial.begin(115200);
  mySerial.begin(57600, SERIAL_8N1, 16, 17);
  
  Serial.println("\n\n=================================");
  Serial.println("  Fingerprint Attendance System  ");
  Serial.println("  with Temperature Control");
  Serial.println("=================================\n");
  
  // Initialize DHT11
// Initialize DHT11
Serial.println("🌡 Initializing DHT11 sensor...");
dht.begin();
pinMode(DHTPIN, INPUT_PULLUP);  // ← ADD THIS LINE (enables internal pull-up)
delay(2000);  // Give DHT11 time to stabilize

  // Test DHT11
  float testTemp = dht.readTemperature();
  if (isnan(testTemp)) {
    Serial.println("⚠ DHT11 not responding properly");
  } else {
    Serial.print("✓ DHT11 initialized - Current temp: ");
    Serial.print(testTemp);
    Serial.println("°C");
  }
  
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.drawBitmap(0, 20, logo_bmp, LOGO_WIDTH, LOGO_HEIGHT, 1);
  display.display();
  delay(1000);
  display.clearDisplay();

  
  // Initialize fingerprint sensor
  Serial.println("👆 Initializing fingerprint sensor...");
  if (finger.verifyPassword()) {
    Serial.println("✓ Fingerprint Sensor Connected");
    beepStop();
  } else {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 0);
    display.println("Sensor");
    display.setCursor(25, 35);
    display.println("Error");
    display.display();
    Serial.println("❌ Unable to find Sensor");
    
    while (1) { 
      beepError();
    }
  }
  
  display.clearDisplay();
  
  // Initialize RTC
  Serial.println("🕐 Initializing RTC...");
  if (!rtc.begin()) {
    Serial.println("❌ RTC not found!");
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("RTC ERROR!");
    display.display();
    while(1) {
      beepError();
    }
  }
  Serial.println("✓ RTC initialized");
  
  // Initialize SPIFFS
  Serial.println("💾 Initializing SPIFFS...");
  if(!SPIFFS.begin(true)){
    Serial.println("❌ SPIFFS Mount Failed!");
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(15, 0);
    display.println("Storage");
    display.setCursor(25, 35);
    display.println("Error");
    display.display();
    
    while(1) { 
      beepError();
    }
  }
  Serial.println("✓ SPIFFS mounted successfully");
 
  // Initialize JSON files
  if (!SPIFFS.exists(employeesPath)) {
    File file = SPIFFS.open(employeesPath, FILE_WRITE);
    if (file) {
      file.println("{}");
      file.close();
      Serial.println("✓ Created employees.json");
    }
  }
  
  if (!SPIFFS.exists(attendancePath)) {
    File file = SPIFFS.open(attendancePath, FILE_WRITE);
    if (file) {
      file.println("[]");
      file.close();
      Serial.println("✓ Created attendance.json");
    }
  }
    // Initialize temperature file
  if (!SPIFFS.exists(temperatureRecordsPath)) {
    File file = SPIFFS.open(temperatureRecordsPath, FILE_WRITE);
    if (file) {
      file.println("[]");
      file.close();
    }
  }
  
  // Read configuration
  Serial.println("⚙ Reading configuration...");
  ssid_ = readFile(SPIFFS, ssidPath);
  pass_ = readFile(SPIFFS, passPath);
  ip_ = readFile(SPIFFS, ipPath);
  gateway_ = readFile(SPIFFS, gatewayPath);
  dispname_ = readFile(SPIFFS, dispnamePath);
  if (dispname_ == "") dispname_ = DEFAULT_displayname;
  wwwid_ = readFile(SPIFFS, wwwidPath);
  if (wwwid_ == "") wwwid_ = DEFAULT_wwwusername;
  wwwpass_ = readFile(SPIFFS, wwwpassPath);
  if (wwwpass_ == "") wwwpass_ = DEFAULT_wwwpassword;
  mdnsdotlocalurl = readFile(SPIFFS, mdnsPath);
  dhcpcheck = readFile(SPIFFS, dhcpcheckPath);
  
  display.drawBitmap(32, 0, logo_wifi, 64, 61, 1);
  display.display();
  display.clearDisplay();
  
  // Connect to WiFi
  Serial.println("📡 Connecting to WiFi...");
  connectwifi();
  
  // Sync RTC with NTP if WiFi connected
  if (WiFi.status() == WL_CONNECTED) {
    syncRTCWithNTP();
  } else {
    Serial.println("⚠ WiFi not connected. RTC will use current time.");
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("No WiFi!");
    display.println("Using RTC time");
    display.display();
    delay(2000);
  }
  
  if (mdnsdotlocalurl == "") mdnsdotlocalurl = DEFAULT_mdns;
  if (!MDNS.begin(mdnsdotlocalurl.c_str())) {
    Serial.println("⚠ Error setting up MDNS responder!");
  }
  MDNS.addService("http", "tcp", 80);
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);
  server.on("/insert", insertRecord);
  server.on("/Settings", Settings);
  server.on("/save", save);
  server.on("/getssid", getssid);
  server.on("/getfpid", getfpid);
  server.on("/getmdns", getmdns);
  server.on("/getip", getip);
  server.on("/login", handleLogin);
  server.on("/signout", logout);
  server.on("/listemployees", listAllEmployees);
  server.on("/attendance", listAttendance);
  server.on("/deleteattendance", deleteAttendanceRecord);
  server.on("/deleteemployee", deleteEmployeeEndpoint);
  server.on("/getroomtemperature", getTemperatureEndpoint);
  server.on("/gettemperaturerecords", getRecentTemperatureRecords);
  server.on("/download/employees", downloadEmployeesJSON);
  server.on("/download/attendance", downloadAttendanceJSON);
  server.on("/download/temperature", downloadTemperatureJSON);
  
  const char *headerkeys[] = {"User-Agent", "Cookie"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);
  server.begin();
  
  Serial.println("\n✓✓✓ System Ready! ✓✓✓");
  Serial.println("👆 Place finger on sensor to clock in/out");
  Serial.println("🌡 Fan control is active\n");
  
  booting = false;
}

// ==================== MAIN LOOP ====================

void loop() {
  server.handleClient();
  controlFanSpeed();  // Check and adjust fan speed continuously
  FingerprintID();

  static unsigned long lastTempRead = 0;
  if (millis() - lastTempRead > 10000) {  // Read every 10 seconds
    float temperature = dht.readTemperature();  // Your sensor reading function
    saveTemperatureRecord(temperature);
    lastTempRead = millis();
  }
  
  // Reconnect WiFi if disconnected (check every 30 minutes)
  if (WiFi.status() != WL_CONNECTED && millis() - lastwificheck > 1800000) {
    connectwifi();
    if (WiFi.status() == WL_CONNECTED) {
      syncRTCWithNTP();
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    // Re-sync RTC with NTP every 24 hours
    if (rtcSynced && millis() - lastNtpSync > 86400000) {
      syncRTCWithNTP();
    }
  }
}

// ==================== JSON STORAGE FUNCTIONS ====================

bool saveEmployee(int fpid, String matricNo, String name, String email, String position) {
  File file = SPIFFS.open(employeesPath, FILE_READ);
  if (!file) {
    Serial.println("❌ Failed to open employees file for reading");
    return false;
  }
  
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.print("❌ Failed to parse employees JSON: ");
    Serial.println(error.c_str());
    return false;
  }
  
  JsonObject employee = doc.createNestedObject(String(fpid));
  employee["matricNo"] = matricNo;
  employee["name"] = name;
  employee["email"] = email;
  employee["position"] = position;
  
  file = SPIFFS.open(employeesPath, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Failed to open employees file for writing");
    return false;
  }
  
  serializeJson(doc, file);
  file.close();
  Serial.println("✓ Employee saved successfully");
  return true;
}

bool getEmployee(int fpid, String &matricNo, String &name, String &email, String &position) {
  File file = SPIFFS.open(employeesPath, FILE_READ);
  if (!file) {
    Serial.println("❌ Failed to open employees file");
    return false;
  }
  
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.print("❌ Failed to parse employees JSON: ");
    Serial.println(error.c_str());
    return false;
  }
  
  String key = String(fpid);
  if (doc.containsKey(key)) {
    matricNo = doc[key]["matricNo"].as<String>();
    name = doc[key]["name"].as<String>();
    email = doc[key]["email"].as<String>();
    position = doc[key]["position"].as<String>();
    return true;
  }
  
  return false;
}

bool deleteEmployee(int fpid) {
  File file = SPIFFS.open(employeesPath, FILE_READ);
  if (!file) return false;
  
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) return false;
  
  doc.remove(String(fpid));
  
  file = SPIFFS.open(employeesPath, FILE_WRITE);
  if (!file) return false;
  
  serializeJson(doc, file);
  file.close();
  Serial.println("✓ Employee deleted successfully");
  return true;
}

bool saveAttendance(int fpid, String date, String time, String matricNo, String name, String email, String position, String punchType) {
  File file = SPIFFS.open(attendancePath, FILE_READ);
  if (!file) {
    Serial.println("❌ Failed to open attendance file for reading");
    return false;
  }
  
  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.print("❌ Failed to parse attendance JSON: ");
    Serial.println(error.c_str());
    return false;
  }
  
  JsonObject record = doc.createNestedObject();
  record["fpid"] = fpid;
  record["date"] = date;
  record["time"] = time;
  record["matricNo"] = matricNo;
  record["name"] = name;
  record["email"] = email;
  record["position"] = position;
  record["punch_type"] = punchType;
  record["timestamp"] = millis();
  
  file = SPIFFS.open(attendancePath, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Failed to open attendance file for writing");
    return false;
  }
  
  serializeJson(doc, file);
  file.close();
  Serial.println("✓ Attendance recorded successfully");
  return true;
}
bool saveTemperatureRecord(float temperature) {
  DateTime now = rtc.now();
  
  // Format date and time
  String dateStr = String(now.day()) + month_name[now.month() - 1] + String(now.year(), DEC);
  
  String timeStr = "";
  if (now.hour() < 10) timeStr += "0";
  timeStr += String(now.hour()) + ":";
  if (now.minute() < 10) timeStr += "0";
  timeStr += String(now.minute()) + ":";
  if (now.second() < 10) timeStr += "0";
  timeStr += String(now.second());
  
  // Read existing records
  File file = SPIFFS.open(temperatureRecordsPath, FILE_READ);
  if (!file) {
    Serial.println("Failed to open temperature file");
    return false;
  }
  
  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.println("Failed to parse temperature JSON");
    return false;
  }
  
  // Add new record
  JsonObject record = doc.createNestedObject();
  record["date"] = dateStr;
  record["time"] = timeStr;
  record["temperature"] = temperature;
  record["timestamp"] = now.unixtime();  // Unix timestamp for easy filtering
  
  // Save back to file
  file = SPIFFS.open(temperatureRecordsPath, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to write temperature file");
    return false;
  }
  
  serializeJson(doc, file);
  file.close();
  
  Serial.println("Temperature saved: " + String(temperature) + "°C");
  return true;
}

int countTodayPunches(int fpid, String date) {
  File file = SPIFFS.open(attendancePath, FILE_READ);
  if (!file) return 0;
  
  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) return 0;
  
  int count = 0;
  JsonArray records = doc.as<JsonArray>();
  for (JsonObject record : records) {
    if (record["fpid"] == fpid && record["date"] == date) {
      count++;
    }
  }
  
  return count;
}

void listAllEmployees() {
  // if (!is_authentified()) {
  //   server.sendHeader("Location", "/login");
  //   server.sendHeader("Cache-Control", "no-cache");
  //   server.send(301);
  //   return;
  // }
  
  File file = SPIFFS.open(employeesPath, FILE_READ);
  if (!file) {
    server.send(500, "application/json", "{\"error\":\"Failed to read employees\"}");
    return;
  }
  
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    server.send(500, "application/json", "{\"error\":\"Failed to parse employees\"}");
    return;
  }
  
  String json = "[";
  bool first = true;
  
  JsonObject employees = doc.as<JsonObject>();
  for (JsonPair kv : employees) {
    if (!first) json += ",";
    json += "{";
    json += "\"fpid\":" + String(kv.key().c_str()) + ",";
    json += "\"matricNo\":\"" + kv.value()["matricNo"].as<String>() + "\",";
    json += "\"name\":\"" + kv.value()["name"].as<String>() + "\",";
    json += "\"email\":\"" + kv.value()["email"].as<String>() + "\",";
    json += "\"position\":\"" + kv.value()["position"].as<String>() + "\"";
    json += "}";
    first = false;
  }
  json += "]";
  
  server.send(200, "application/json", json);
}

void resetEmployeesJSON() {
  File file = SPIFFS.open(employeesPath, FILE_WRITE);
  if (file) {
    file.println("{}");
    file.close();
    Serial.println("✓ employees.json reset to empty");
  }
}

void listAttendance() {
  // if (!is_authentified()) {
  //   server.sendHeader("Location", "/login");
  //   server.sendHeader("Cache-Control", "no-cache");
  //   server.send(301);
  //   return;
  // }
  
  File file = SPIFFS.open(attendancePath, FILE_READ);
  if (!file) {
    server.send(500, "application/json", "{\"error\":\"Failed to read attendance\"}");
    return;
  }
  
  String content = file.readString();
  file.close();
  
  server.send(200, "application/json", content);
}


void deleteAttendanceRecord() {
  Serial.println("=== DELETING ALL ATTENDANCE RECORDS ===");
  
  // Simply overwrite the file with an empty array
  File file = SPIFFS.open(attendancePath, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Failed to open attendance file for writing");
    server.send(500, "application/json", "{\"error\":\"Failed to open file\"}");
    return;
  }
  
  // Write empty JSON array
  file.println("[]");
  file.close();
  
  Serial.println("✓ All attendance records deleted");
  server.send(200, "application/json", "{\"success\":true,\"message\":\"All attendance records deleted\"}");
}

void deleteEmployeeEndpoint() {
  // if (!is_authentified()) {
  //   server.sendHeader("Location", "/login");
  //   server.sendHeader("Cache-Control", "no-cache");
  //   server.send(301);
  //   return;
  // }
  
  if (!server.hasArg("fpid")) {
    server.send(400, "application/json", "{\"error\":\"Missing fpid parameter\"}");
    return;
  }
  
  int fpid = server.arg("fpid").toInt();
  
  // Delete from fingerprint sensor
  uint8_t result = deleteFingerprint(fpid);
  
  if (result == FINGERPRINT_OK) {
    // Delete from JSON storage
    if (deleteEmployee(fpid)) {
      server.send(200, "application/json", "{\"success\":true,\"message\":\"Employee deleted successfully\"}");
      Serial.println("✓ Employee deleted: FPID " + String(fpid));
      
      // Display confirmation on OLED
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
      display.setTextSize(1);
      oledDisplayCenter("Employee Deleted", 0, 50);
      oledDisplayCenter("ID: " + String(fpid), 0, 58);
      display.display();
      beepSuccess();
      delay(2000);
      display.clearDisplay();
    } else {
      server.send(500, "application/json", "{\"error\":\"Failed to delete from storage\"}");
      Serial.println("❌ Failed to delete employee from storage");
    }
  } else {
    server.send(500, "application/json", "{\"error\":\"Failed to delete fingerprint\"}");
    Serial.println("❌ Failed to delete fingerprint from sensor");
    
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
    display.setTextSize(1);
    oledDisplayCenter("Delete Failed", 0, 50);
    oledDisplayCenter("Try Again", 0, 58);
    display.display();
    beepFailure();
    delay(2000);
    display.clearDisplay();
  }
}

void getTemperatureEndpoint() {
  float temp = dht.readTemperature();
  Serial.println(temp);

  server.send(200, "text/plain", String(temp));
}

// Function to see ALL records without any filtering
void getRecentTemperatureRecords() {
  File file = SPIFFS.open(temperatureRecordsPath, FILE_READ);
  if (!file) {
    server.send(500, "application/json", "{\"error\":\"Failed to open file\"}");
    return;
  }
  
  // Just read and send the entire file content
  String content = file.readString();
  file.close();
  
  Serial.println("Raw file content:");
  Serial.println(content);
  
  server.send(200, "application/json", content);
}


// ==================== HELPER FUNCTIONS ====================

void oledDisplayCenter(String text, int x, int y) {
  int16_t x1, y1;
  uint16_t width, height;
  display.getTextBounds(text, x, y, &x1, &y1, &width, &height);
  display.setCursor((SCREEN_WIDTH - width) / 2, y);
  display.print(text);
}

String readFile(fs::FS &fs, const char *path) {
  File file = fs.open(path);
  if (!file || file.isDirectory()) return String();
  String fileContent;
  while (file.available()) {
    fileContent = file.readStringUntil('\n');
    break;
  }
  return fileContent;
}

void writeFile(fs::FS &fs, const char *path, const char *message) {
  File file = fs.open(path, FILE_WRITE);
  if (!file) return;
  file.print(message);
}

void appendFile(fs::FS &fs, const char *path, const char *message) {
  File file = fs.open(path, FILE_APPEND);
  if (!file) return;
  file.println(message);
}

bool loadFromSPIFFS(String path) {
  String dataType = "text/html";
  if (SPIFFS.exists(path)) {
    File dataFile = SPIFFS.open(path, "r");
    if (!dataFile) return false;
    if (server.streamFile(dataFile, dataType) != dataFile.size()) {
      Serial.println("⚠ Sent less data than expected!");
    }
    dataFile.close();
    return true;
  }
  handleNotFound();
  return false;
}

bool is_authentified() {
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    if (cookie.indexOf("ESPSESSIONID=1") != -1) return true;
  }
  return false;
}

void handleLogin() {
  String msg;
  if (server.hasArg("DISCONNECT")) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Set-Cookie", "ESPSESSIONID=0");
    server.send(301);
    return;
  }
  if (server.hasArg("USERNAME") && server.hasArg("PASSWORD")) {
    if (server.arg("USERNAME") == wwwid_ && server.arg("PASSWORD") == wwwpass_) {
      server.sendHeader("Location", "/");
      server.sendHeader("Cache-Control", "no-cache");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=1");
      server.send(301);
      return;
    }
    msg = "Wrong username/password! try again.";
  }
  loadFromSPIFFS("/Login.html");
}

void logout() {
  server.sendHeader("Location", "/login");
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Set-Cookie", "ESPSESSIONID=0");
  server.send(301);
}

void handleRoot() {
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  loadFromSPIFFS("/db.html");
}

void Settings() {
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  loadFromSPIFFS("/Settings.html");
}

void handleNotFound() {
  String message = "File Not Found\n\nURI: " + server.uri() + "\nMethod: " + (server.method() == HTTP_GET ? "GET" : "POST") + "\nArguments: " + server.args() + "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

// void insertRecord() {
//   web_content = "";
//   String eemployee_id = server.arg("memployee_id");
//   String ename = server.arg("mname");
//   String eemail_id = server.arg("memail_id");
//   String epos = server.arg("mpos");
//   String efpid = server.arg("mfpid");
//   id = efpid.toInt();
  
//   uint8_t result = getFingerprintEnroll();
//   if (result == FINGERPRINT_OK) {
//     if (saveEmployee(id, eemployee_id, ename, eemail_id, epos)) {
//       web_content += "OK";
//       display.clearDisplay();
//       display.setTextColor(SSD1306_WHITE);
//       display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
//       oledDisplayCenter("Enroll Success!!", 0, 60);
//       display.display();
//       beepSuccess();
//       delay(1000);
//     } else {
//       web_content += "STORAGE_ERROR";
//       beepFailure();
//     }
//   } else {
//     web_content += "FAIL";
//     display.clearDisplay();
//     display.setTextColor(SSD1306_WHITE);
//     display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
//     oledDisplayCenter("Enrollment Failed!", 0, 60);
//     display.display();
//     beepFailure();
//     delay(2000);
//   }
//   server.send(200, "text/html", web_content);
// }
void insertRecord() {
  web_content = "";
  String eemployee_id = server.arg("memployee_id");
  String ename = server.arg("mname");
  String eemail_id = server.arg("memail_id");
  String epos = server.arg("mpos");
  String efpid = server.arg("mfpid");
  id = efpid.toInt();
  
  Serial.println("=== ENROLLMENT STARTED ===");
  
  uint8_t result = getFingerprintEnroll();
  
  if (result == FINGERPRINT_OK) {
    if (saveEmployee(id, eemployee_id, ename, eemail_id, epos)) {
      web_content += "OK";
      
      Serial.println("✓ Enrollment complete, now clearing system...");
      
      // Show success message
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
      oledDisplayCenter("Enroll Success!!", 0, 60);
      display.display();
      beepSuccess();
      delay(2000);
      
      // CRITICAL: Force user to remove finger
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      oledDisplayCenter("REMOVE FINGER", 0, 20);
      oledDisplayCenter("NOW!", 0, 35);
      display.display();
      
      Serial.println("⏳ Waiting for finger removal...");
      
      // Aggressive wait for finger removal
      bool fingerRemoved = false;
      int maxWait = 100; // 10 seconds max
      int waitCount = 0;
      
      while (!fingerRemoved && waitCount < maxWait) {
        int noFingerCount = 0;
        
        // Check 10 times in a row
        for (int i = 0; i < 10; i++) {
          if (finger.getImage() == FINGERPRINT_NOFINGER) {
            noFingerCount++;
          }
          delay(50);
        }
        
        if (noFingerCount >= 8) {  // At least 8 out of 10 must be "no finger"
          fingerRemoved = true;
        }
        
        waitCount++;
      }
      
      if (fingerRemoved) {
        Serial.println("✓ Finger removed!");
      } else {
        Serial.println("⚠ Timeout waiting for finger removal");
      }
      
      // Clear sensor buffer completely
      Serial.println("🧹 Clearing sensor...");
      for (int i = 0; i < 10; i++) {
        finger.getImage();
        delay(50);
      }
      
      // Show completion message
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      oledDisplayCenter("Enrollment", 0, 20);
      oledDisplayCenter("Complete!", 0, 30);
      oledDisplayCenter("", 0, 40);
      oledDisplayCenter("Ready for", 0, 45);
      oledDisplayCenter("Attendance", 0, 55);
      display.display();
      delay(2000);
      
      // Force clear display
      display.clearDisplay();
      display.display();
      
      // Reset lastDisplayUpdate to force immediate redraw of main screen
      lastDisplayUpdate = 0;
      
      Serial.println("=== SYSTEM READY FOR ATTENDANCE ===\n");
      
    } else {
      web_content += "STORAGE_ERROR";
      Serial.println("❌ Failed to save employee");
      beepFailure();
    }
  } else {
    web_content += "FAIL";
    Serial.println("❌ Enrollment failed");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
    oledDisplayCenter("Enrollment Failed!", 0, 60);
    display.display();
    beepFailure();
    delay(2000);
    
    // Clear display after failure too
    display.clearDisplay();
    display.display();
    lastDisplayUpdate = 0;
  }
  
  server.send(200, "text/html", web_content);
}

void save() {
  web_content = "";
  String _ssid = server.arg("ssid");
  String _password = server.arg("password");
  String _mdns = server.arg("mdns");
  String _aip = server.arg("aip");
  String _mip = server.arg("mip");
  String _gateway = server.arg("gateway");
  String _dispname = server.arg("dispname");
  String _wwwid = server.arg("wwwid");
  String _wwwpass = server.arg("wwwpass");
  
  if (_ssid != "") writeFile(SPIFFS, ssidPath, _ssid.c_str());
  if (_password != "") writeFile(SPIFFS, passPath, _password.c_str());
  if (_mdns != "") writeFile(SPIFFS, mdnsPath, _mdns.c_str());
  if (_aip != "") writeFile(SPIFFS, dhcpcheckPath, _aip.c_str());
  if (_mip != "") writeFile(SPIFFS, ipPath, _mip.c_str());
  if (_gateway != "") writeFile(SPIFFS, gatewayPath, _gateway.c_str());
  if (_dispname != "") writeFile(SPIFFS, dispnamePath, _dispname.c_str());
  if (_wwwid != "") writeFile(SPIFFS, wwwidPath, _wwwid.c_str());
  if (_wwwpass != "") writeFile(SPIFFS, wwwpassPath, _wwwpass.c_str());
  
  String dtd = server.arg("dtd");
  String dtm = server.arg("dtm");
  String dty = server.arg("dty");
  String tmh = server.arg("tmh");
  String tmm = server.arg("tmm");
  String tms = server.arg("tms");
  String tmapm = server.arg("tmapm");
  
  if (dtd != "1" || dtm != "1" || dty != "2022") {
    year_ = dty.toInt();
    month_ = dtm.toInt();
    day_ = dtd.toInt();
    int ampm = tmapm.toInt();
    if (ampm == 2 && tmh.toInt() < 12) {
      hour24_ = tmh.toInt() + 12;
    } else if (ampm == 1 && tmh.toInt() == 12) {
      hour24_ = 0;
    } else {
      hour24_ = tmh.toInt();
    }
    minute_ = tmm.toInt();
    second_ = tms.toInt();
    rtc.adjust(DateTime(year_, month_, day_, hour24_, minute_, second_));
  }
  
  web_content += "OK";
  server.send(200, "text/html", web_content);
  ESP.restart();
}

void getssid() { server.send(200, "text/plain", ssid_); }
void getmdns() { server.send(200, "text/plain", mdnsdotlocalurl); }
void getip() { server.send(200, "text/plain", ip_); }
void getfpid() { 
  File file = SPIFFS.open(employeesPath, FILE_READ);
  if (!file) {
    server.send(200, "text/plain", "1");
    return;
  }
  
  DynamicJsonDocument doc(8192);
  deserializeJson(doc, file);
  file.close();
  
  int maxId = 0;
  JsonObject employees = doc.as<JsonObject>();
  for (JsonPair kv : employees) {
    int id = String(kv.key().c_str()).toInt();
    if (id > maxId) maxId = id;
  }
  
  server.send(200, "text/plain", String(maxId + 1));
}

// ==================== FINGERPRINT IDENTIFICATION ====================

int FingerprintID() {
  DateTime now = rtc.now();
  uint8_t p = finger.getImage();
  
  if (p != FINGERPRINT_OK) {
    // Update display every 500ms to avoid flickering
    if (millis() - lastDisplayUpdate > 500) {
      display.clearDisplay();
      display.setFont(&Roboto_Bold_10);
      display.setTextColor(SSD1306_WHITE);
      
      // Top row: Date
      display.setCursor(0, 10);
      display.println(nameoftheday[now.dayOfTheWeek()]);
      display.setCursor(25, 10);
      display.println(now.day());
      display.setCursor(42, 10);
      display.println(month_name[now.month() - 1]);
      display.setCursor(67, 10);
      display.println(now.year(), DEC);
      
      // WiFi indicator
      if (WiFi.status() == WL_CONNECTED) {
        display.drawBitmap(110, 0, wifi_bmp, 12, 10, 1);
      } else {
        display.drawBitmap(110, 0, nowifi_bmp, 12, 10, 1);
      }
      
      // Big clock
      display.setFont(&DSEG7_Classic_Bold_30);
      display.setCursor(0, 48);
      if (now.hour() < 10 || (now.hour() > 12 && now.hour() < 22)) display.print("0");
      if (now.hour() < 13) display.print(now.hour()); else display.print(now.hour() - 12);
      if ((now.second() % 2) == 0) display.print(":"); else display.print(" ");
      if (now.minute() < 10) display.print("0");
      display.print(now.minute());
      display.setFont(&Roboto_Bold_10);
      if (now.hour() < 13) display.print("AM");
      display.print("PM");
      
      // Bottom row: Temperature and Fan Speed (instead of display name)
      display.setFont();
      display.setTextSize(1);
      display.setCursor(0, 56);
      
      if (currentTemp > 0) {
        display.print("T:");
        display.print(currentTemp, 1);
        display.print("C|F:");
        display.print(currentFanSpeed);
        display.print("/255");
      } else {
        display.print("Temp: --  Fan: OFF");
      }
      
      display.display();
      lastDisplayUpdate = millis();
    }
    return -1;
  }
  
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    display.clearDisplay();
display.setTextColor(SSD1306_WHITE);
display.drawBitmap(47, 0, messy_bmp, 35, 45, 1);
display.setTextSize(1);  // Ensure small text
oledDisplayCenter("Messy Image", 0, 50);
oledDisplayCenter("Try Again", 0, 58);
display.display();
    Serial.println("⚠ Messy Image Try Again");
    beepFailure();
    delay(1500);
    display.clearDisplay();
    return -1;
  }
  
  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
    display.setTextSize(1);
oledDisplayCenter("Invalid ID", 0, 50);
oledDisplayCenter("Try Again", 0, 58);
    display.display();
    Serial.println("❌ Not Valid Finger");
    beepFailure();
    delay(500);
    display.clearDisplay();
    return -1;
  }
  
  Empfid = String(finger.fingerID);
  matricNo = "EMP" + String(finger.fingerID);
  Empname = "Student " + String(finger.fingerID);
  EmpEmail = "emp" + String(finger.fingerID) + "@example.com";
  EmpPos = "Staff";
  
  if (!getEmployee(finger.fingerID, matricNo, Empname, EmpEmail, EmpPos)) {
    Serial.println("⚠ Student not found in storage, using defaults");
  }
  
  String dateStr = String(now.day()) + month_name[now.month() - 1] + String(now.year(), DEC);
  
  int punch_count = countTodayPunches(finger.fingerID, dateStr);
  String att;
  if (punch_count == 0) {
    att = "Sign In";
  } else if (punch_count == 1) {
    att = "Sign Out";
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
    display.setTextSize(1);
oledDisplayCenter("Already Signed", 0, 50);
oledDisplayCenter("Out", 0, 58);
    display.display();
    Serial.println("⚠ Already Signed Out");
    beepFailure();
    delay(2000);
    display.clearDisplay();
    return -1;
  }
  
  String hr = "";
  if (now.hour() < 10 || (now.hour() > 12 && now.hour() < 22)) hr += "0";
  if (now.hour() < 13) hr += String(now.hour()); else hr += String(now.hour() - 12);
  if (now.minute() < 10) hr += ".0" + String(now.minute()); else hr += "." + String(now.minute());
  if (now.hour() < 12) hr += " AM"; else hr += " PM";
  
  if (!saveAttendance(finger.fingerID, dateStr, hr, matricNo, Empname, EmpEmail, EmpPos, att)) {
    Serial.println("❌ Failed to save attendance record");
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
  display.setTextSize(1);
oledDisplayCenter("Finger Print", 0, 50);
oledDisplayCenter("Verified", 0, 58);
  display.display();
  Serial.println("✓ Finger Print Verified");
  delay(1000);
  
  display.clearDisplay();
  display.drawBitmap(0, 10, welcome_bmp, 128, 17, 1);
  oledDisplayCenter(Empname, 0, 50);
  display.display();
  Serial.print("👋 Welcome: ");
  Serial.println(Empname);
  Serial.print("📋 Attendance: ");
  Serial.println(att);
  beepSuccess();
  delay(1000);
  display.clearDisplay();
  return finger.fingerID;
}

// ==================== FINGERPRINT ENROLLMENT ====================

uint8_t deleteFingerprint(uint8_t id) {
  uint8_t p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("✓ Deleted!");
    deleteEmployee(id);
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("❌ Communication error");
  } else if (p == FINGERPRINT_BADLOCATION) {
    Serial.println("❌ Could not delete in that location");
  } else if (p == FINGERPRINT_FLASHERR) {
    Serial.println("❌ Error writing to flash");
  } else {
    Serial.println("❌ Unknown error: 0x" + String(p, HEX));
  }
  return p;
}

// uint8_t getFingerprintEnroll() {
//   display.clearDisplay();
//   display.setTextColor(SSD1306_WHITE);
//   display.drawBitmap(47, 0, finger_bmp, 35, 45, 1);
//   display.setTextSize(1);
//   display.setCursor(0, 50);
//   display.println("Place Finger");
//   display.setCursor(0, 58);
//   display.println("to Enroll");
//   display.display();
    
//   int p = -1;
//   while (p != FINGERPRINT_OK) {
//     p = finger.getImage();
//     if (p == FINGERPRINT_OK) {
//       Serial.println("✓ Image taken");
//       display.clearDisplay();
//       display.setTextColor(SSD1306_WHITE);
//       display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
//       oledDisplayCenter("Image taken", 0, 60);
//       display.display();
//       beepStop();
//       tone(Buzzer, 2000, 200);
//       delay(200);
//       noTone(Buzzer);
//       delay(300);
//     } else if (p == FINGERPRINT_NOFINGER) {
//       Serial.print(".");
//     } else {
//       Serial.println("❌ Error");
//       display.clearDisplay();
//       display.setTextColor(SSD1306_WHITE);
//       display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
//       oledDisplayCenter("Error", 0, 60);
//       display.display();
//       beepFailure();
//       return p;
//     }
//   }
  
//   p = finger.image2Tz(1);
//   if (p != FINGERPRINT_OK) {
//     Serial.println("❌ Image conversion error");
//     display.clearDisplay();
//     display.setTextColor(SSD1306_WHITE);
//     display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
//     oledDisplayCenter("Image conversion error", 0, 60);
//     display.display();
//     beepFailure();
//     return p;
//   }
  
//   Serial.println("🖐 Remove finger");
//   display.clearDisplay();
//   display.setTextColor(SSD1306_WHITE);
//   display.drawBitmap(47, 0, finger_bmp, 35, 45, 1);
//   oledDisplayCenter("Remove finger", 0, 60);
//   display.display();
//   tone(Buzzer, 1500, 200);
//   delay(200);
//   noTone(Buzzer);
//   delay(800);
  
//   p = 0;
//   while (p != FINGERPRINT_NOFINGER) p = finger.getImage();
  
//   Serial.println("👆 Place same finger again");
//   display.clearDisplay();
//   display.setTextColor(SSD1306_WHITE);
//   display.drawBitmap(47, 0, finger_bmp, 35, 45, 1);
//   display.setTextSize(1);
//   oledDisplayCenter("Place same", 0, 50);
//   oledDisplayCenter("finger again", 0, 58);
//     display.display();
  
//   while (p != FINGERPRINT_OK) {
//     p = finger.getImage();
//     if (p == FINGERPRINT_OK) {
//       Serial.println("✓ Image taken");
//       display.clearDisplay();
//       display.setTextColor(SSD1306_WHITE);
//       display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
//       oledDisplayCenter("Image taken", 0, 60);
//       display.display();
//       tone(Buzzer, 2000, 200);
//       delay(200);
//       noTone(Buzzer);
//       delay(300);
//     } else if (p == FINGERPRINT_NOFINGER) {
//       Serial.print(".");
//     } else {
//       Serial.println("❌ Error");
//       display.clearDisplay();
//       display.setTextColor(SSD1306_WHITE);
//       display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
//       oledDisplayCenter("Error", 0, 60);
//       display.display();
//       beepFailure();
//       return p;
//     }
//   }
  
//   p = finger.image2Tz(2);
//   if (p != FINGERPRINT_OK) {
//     Serial.println("❌ Image conversion error");
//     display.clearDisplay();
//     display.setTextColor(SSD1306_WHITE);
//     display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
//     oledDisplayCenter("Image conversion error", 0, 60);
//     display.display();
//     beepFailure();
//     return p;
//   }
  
//   p = finger.createModel();
//   if (p == FINGERPRINT_OK) {
//     Serial.println("✓ Prints matched!");
//     display.clearDisplay();
//     display.setTextColor(SSD1306_WHITE);
//     display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
//     oledDisplayCenter("Prints matched!", 0, 60);
//     display.display();
//     tone(Buzzer, 2200, 300);
//     delay(300);
//     noTone(Buzzer);
//     delay(200);
//   } else if (p == FINGERPRINT_ENROLLMISMATCH) {
//     Serial.println("❌ Fingerprints did not match. Try again.");
//     display.clearDisplay();
//     display.setTextColor(SSD1306_WHITE);
//     display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
//     display.setTextSize(1);
//   oledDisplayCenter("Fingers didn't", 0, 50);
//   oledDisplayCenter("match", 0, 58);
//     display.display();
//     beepFailure();
//     delay(1500);
//     return p;
//   } else {
//     Serial.println("❌ Error creating model");
//     display.clearDisplay();
//     display.setTextColor(SSD1306_WHITE);
//     display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
//     oledDisplayCenter("Error creating model", 0, 60);
//     display.display();
//     beepFailure();
//     return p;
//   }
  
//   p = finger.storeModel(id);
//   if (p == FINGERPRINT_OK) {
//     Serial.println("✓ Stored!");
//     display.clearDisplay();
//     display.setTextColor(SSD1306_WHITE);
//     display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
//     oledDisplayCenter("Image Stored!", 0, 60);
//     display.display();
//     tone(Buzzer, 2500, 400);
//     delay(400);
//     noTone(Buzzer);
//     delay(100);
//     return FINGERPRINT_OK;
//   } else {
//     Serial.println("❌ Error storing model");
//     display.clearDisplay();
//     display.setTextColor(SSD1306_WHITE);
//     display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
//     oledDisplayCenter("Error storing model", 0, 60);
//     display.display();
//     beepFailure();
//     return p;
//   }
// }


uint8_t getFingerprintEnroll() {
  // Clear any residual data from sensor
  finger.getImage();
  delay(100);
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.drawBitmap(47, 0, finger_bmp, 35, 45, 1);
  display.setTextSize(1);
  display.setCursor(0, 50);
  display.println("Place Finger");
  display.setCursor(0, 58);
  display.println("to Enroll");
  display.display();
  
  Serial.println("👆 Waiting for finger to enroll...");
  
  int p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      Serial.println("✓ Image taken");
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
      oledDisplayCenter("Image taken", 0, 60);
      display.display();
      beepStop();
      tone(Buzzer, 2000, 200);
      delay(200);
      noTone(Buzzer);
      delay(300);
    } else if (p == FINGERPRINT_NOFINGER) {
      Serial.print(".");
      delay(100);  // Small delay to avoid flooding
    } else {
      Serial.println("❌ Error");
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
      oledDisplayCenter("Error", 0, 60);
      display.display();
      beepFailure();
      return p;
    }
  }
  
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println("❌ Image conversion error");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
    oledDisplayCenter("Image conversion error", 0, 60);
    display.display();
    beepFailure();
    return p;
  }
  
  Serial.println("🖐 Remove finger");
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.drawBitmap(47, 0, finger_bmp, 35, 45, 1);
  oledDisplayCenter("Remove finger", 0, 60);
  display.display();
  tone(Buzzer, 1500, 200);
  delay(200);
  noTone(Buzzer);
  delay(800);
  
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    delay(50);
  }
  
  Serial.println("👆 Place same finger again");
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.drawBitmap(47, 0, finger_bmp, 35, 45, 1);
  display.setTextSize(1);
  oledDisplayCenter("Place same", 0, 50);
  oledDisplayCenter("finger again", 0, 58);
  display.display();
  
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      Serial.println("✓ Image taken");
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
      oledDisplayCenter("Image taken", 0, 60);
      display.display();
      tone(Buzzer, 2000, 200);
      delay(200);
      noTone(Buzzer);
      delay(300);
    } else if (p == FINGERPRINT_NOFINGER) {
      Serial.print(".");
      delay(100);
    } else {
      Serial.println("❌ Error");
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
      oledDisplayCenter("Error", 0, 60);
      display.display();
      beepFailure();
      return p;
    }
  }
  
  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println("❌ Image conversion error");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
    oledDisplayCenter("Image conversion error", 0, 60);
    display.display();
    beepFailure();
    return p;
  }
  
  p = finger.createModel();
  if (p == FINGERPRINT_OK) {
    Serial.println("✓ Prints matched!");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
    oledDisplayCenter("Prints matched!", 0, 60);
    display.display();
    tone(Buzzer, 2200, 300);
    delay(300);
    noTone(Buzzer);
    delay(200);
  } else if (p == FINGERPRINT_ENROLLMISMATCH) {
    Serial.println("❌ Fingerprints did not match. Try again.");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
    display.setTextSize(1);
    oledDisplayCenter("Fingers didn't", 0, 50);
    oledDisplayCenter("match", 0, 58);
    display.display();
    beepFailure();
    delay(1500);
    return p;
  } else {
    Serial.println("❌ Error creating model");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
    oledDisplayCenter("Error creating model", 0, 60);
    display.display();
    beepFailure();
    return p;
  }
  
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("✓ Stored!");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, ok_bmp, 35, 45, 1);
    oledDisplayCenter("Image Stored!", 0, 60);
    display.display();
    tone(Buzzer, 2500, 400);
    delay(400);
    noTone(Buzzer);
    
    // CRITICAL: Clear the sensor buffer after enrollment
    Serial.println("🧹 Clearing sensor buffer...");
    delay(500);
    finger.getImage();  // Clear any residual data
    delay(100);
    
    // Reset display to ready state
    display.clearDisplay();
    display.display();
    
    Serial.println("✓ Enrollment complete - sensor ready for normal operation");
    
    return FINGERPRINT_OK;
  } else {
    Serial.println("❌ Error storing model");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawBitmap(47, 0, invalid_bmp, 35, 45, 1);
    oledDisplayCenter("Error storing model", 0, 60);
    display.display();
    beepFailure();
    return p;
  }
}

// ==================== WIFI CONNECTION ====================

void connectwifi() {
  if (ssid_ == "") {
    Serial.println("⚠ No wifi config found, connecting to default WiFi: " + String(DEFAULT_WIFI_SSID));
    WiFi.mode(WIFI_STA);
    WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
      delay(250);
      Serial.print(".");
    }
  } else {
    Serial.println("📡 Connecting to WiFi: " + ssid_);
    WiFi.mode(WIFI_STA);
    if (dhcpcheck == "2" && ip_ != "" && gateway_ != "") {
      localIP.fromString(ip_);
      gatewayIP.fromString(gateway_);
      WiFi.config(localIP, gatewayIP, subnetMask);
    }
    WiFi.begin(ssid_.c_str(), pass_.c_str());
    int connectcount = 0;
    while (WiFi.status() != WL_CONNECTED && connectcount < 50) {
      delay(250);
      Serial.print(".");
      connectcount++;
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ Connected. IP address: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n❌ Failed to connect to WiFi");
  }
  lastwificheck = millis();
}



// ==================== DOWNLOAD JSON ENDPOINTS ====================

void downloadEmployeesJSON() {
  // if (!is_authentified()) {
  //   server.sendHeader("Location", "/login");
  //   server.sendHeader("Cache-Control", "no-cache");
  //   server.send(301);
  //   return;
  // }
  
  File file = SPIFFS.open(employeesPath, FILE_READ);
  if (!file) {
    server.send(500, "application/json", "{\"error\":\"Failed to open employees file\"}");
    return;
  }
  
  // Set headers for download
  server.sendHeader("Content-Type", "application/json");
  server.sendHeader("Content-Disposition", "attachment; filename=employees.json");
  
  // Stream the file content
  server.streamFile(file, "application/json");
  file.close();
  
  Serial.println("✓ Employees JSON downloaded");
}

void downloadAttendanceJSON() {
  // if (!is_authentified()) {
  //   server.sendHeader("Location", "/login");
  //   server.sendHeader("Cache-Control", "no-cache");
  //   server.send(301);
  //   return;
  // }
  
  File file = SPIFFS.open(attendancePath, FILE_READ);
  if (!file) {
    server.send(500, "application/json", "{\"error\":\"Failed to open attendance file\"}");
    return;
  }
  
  // Set headers for download
  server.sendHeader("Content-Type", "application/json");
  server.sendHeader("Content-Disposition", "attachment; filename=attendance.json");
  
  // Stream the file content
  server.streamFile(file, "application/json");
  file.close();
  
  Serial.println("✓ Attendance JSON downloaded");
}

void downloadTemperatureJSON() {
  // if (!is_authentified()) {
  //   server.sendHeader("Location", "/login");
  //   server.sendHeader("Cache-Control", "no-cache");
  //   server.send(301);
  //   return;
  // }
  
  File file = SPIFFS.open(temperatureRecordsPath, FILE_READ);
  if (!file) {
    server.send(500, "application/json", "{\"error\":\"Failed to open temperature records file\"}");
    return;
  }
  
  // Set headers for download
  server.sendHeader("Content-Type", "application/json");
  server.sendHeader("Content-Disposition", "attachment; filename=temperature_records.json");
  
  // Stream the file content
  server.streamFile(file, "application/json");
  file.close();
  
  Serial.println("✓ Temperature records JSON downloaded");
}