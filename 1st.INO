// --- BUG FIX FOR ESP32 VERSION 3.x ---
#include <unordered_map> 
// -------------------------------------

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <DHT.h>
#include <Adafruit_ADS1X15.h>
#include <BH1750.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Firebase_ESP_Client.h>
#include <time.h> 

// Provide the token generation process info
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- 1. FIREBASE CONFIGURATION ---
const char* ssid = "Samsung galaxy s24";
const char* password = "Dilshan@123456";
#define API_KEY "AIzaSyBQkUI1zFx28AJ5PEyCNVMR0xGx1l0lZ9Y"
#define FIREBASE_PROJECT_ID "researchproject-606d4"

FirebaseData fbdo1;
FirebaseData fbdo2;
FirebaseData fbdoStatus;
FirebaseAuth auth;
FirebaseConfig config;

// --- 2. HARDWARE PINS ---
#define I2C_SDA 8
#define I2C_SCL 9
#define SD_CS 10
#define SD_MOSI 11
#define SD_MISO 13
#define SD_SCK 12
#define DHT1_PIN 4
#define DHT2_PIN 6
#define DHTTYPE DHT11
#define MAX7400_CLK_PIN 16
#define POWER_SWITCH_PIN 5 

// --- 3. OBJECTS ---
DHT dht1(DHT1_PIN, DHTTYPE);
DHT dht2(DHT2_PIN, DHTTYPE);
Adafruit_ADS1115 ads; 
BH1750 lightMeter1; 
BH1750 lightMeter2; 
RTC_DS3231 rtc;
WebServer server(80); 

// --- 4. SYSTEM STATE & WEB LOGS ---
#define MAX_WEB_LOGS 20
String webLogs[MAX_WEB_LOGS];
int webLogIndex = 0;

struct CageState {
  String name;
  String configPath;
  bool isRunning;
  unsigned long endTime;   
  unsigned long lastSampleTime;
  String lastUpdateID;
  int durationMin;
  int timeGapSec;
  String variety;
  String salinity;
  String batch;
  String historyId;
  bool isBootSyncDone; 
};

CageState cage1 = {"Cage1", "settings/cage1", false, 0, 0, "", 0, 0, "", "", "", "", false};
CageState cage2 = {"Cage2", "settings/cage2", false, 0, 0, "", 0, 0, "", "", "", "", false};

// --- TIMERS ---
unsigned long lastOnlineReport = 0;
unsigned long lastWiFiCheck = 0; 
unsigned long lastFirebaseCheck = 0; // Moved here so we can control it on reconnect
bool isAdapterPowerOn = true; 

// --- FUNCTION PROTOTYPES ---
void checkCommands(CageState &cage, FirebaseData &fbdo);
void managePowerSource();
void collectData(CageState &cage, int cageNum);
void logToSDBatch(CageState &cage, String dateStr, String timeStr, float t, float h, float lux, float elecMV, float soilMV);
int readADS_Average(uint8_t channel, int samples);
float readLightAverage(BH1750 &sensor);
void verifyRTC(); 
void addWebLog(String msg);
void handleRoot();
void handleDownload();
void handleDelete();
void handleLogs();

// -------------------- SETUP --------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(); 

  // 1. Initialize Power Switch 
  pinMode(POWER_SWITCH_PIN, OUTPUT);
  digitalWrite(POWER_SWITCH_PIN, HIGH); 

  // 2. Setup Base I2C & RTC 
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!rtc.begin()) addWebLog("RTC not found!");

  // 3. Setup MAX7400 Clock
  ledcAttach(MAX7400_CLK_PIN, 300, 8); 
  ledcWrite(MAX7400_CLK_PIN, 127); 

  // 4. Initialize Sensors ONCE
  dht1.begin();
  dht2.begin();
  if (ads.begin()) ads.setGain(GAIN_ONE);
  lightMeter1.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire); 
  lightMeter2.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x5C, &Wire);

  // --- DEBUG STEP 1: Initialize SD Card ---
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    addWebLog("SD Card Mount Failed!");
  } else {
    addWebLog("SD Card Initialized!");
  }
  delay(500);

  // --- DEBUG STEP 2: Connect WiFi (Non-Blocking) ---
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    addWebLog("WiFi Connected!");
    addWebLog("Access Local Host at IP: " + WiFi.localIP().toString()); 
    verifyRTC(); 
  } else {
    addWebLog("WiFi Failed. Starting in OFFLINE mode.");
  }
  delay(500);

  // 7. Setup Local Web Server Handlers
  server.on("/", handleRoot);
  server.on("/download", handleDownload);
  server.on("/delete", handleDelete);
  server.on("/logs", handleLogs);
  server.begin();

  // 8. Setup Firebase
  config.api_key = API_KEY;
  config.signer.test_mode = true;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(false); 

  // --- DEBUG STEP 5: Cage Initialization ---
  addWebLog("Cage1 Initialized safely. Staying IDLE.");
  delay(200);
  addWebLog("Cage2 Initialized safely. Staying IDLE.");
}

// -------------------- MAIN LOOP --------------------
void loop() {
  server.handleClient(); // Local web server always runs

  static bool wasOffline = false; 

  // --- WIFI AUTO RECONNECT LOGIC ---
  if (WiFi.status() != WL_CONNECTED) {
      wasOffline = true; // Mark that we are offline
      if (millis() - lastWiFiCheck > 30000) { // Try to connect every 30 seconds
          lastWiFiCheck = millis();
          WiFi.disconnect();
          WiFi.begin(ssid, password);
          addWebLog("WiFi Disconnected. Attempting Reconnect...");
      }
  } else {
      // --- WAKE UP FIX ---
      if (wasOffline) {
          wasOffline = false; 
          addWebLog("WiFi RECONNECTED! New IP: " + WiFi.localIP().toString());
          server.begin(); // Refresh the local web server
          
          // Clear dead session buffers
          fbdo1.clear();
          fbdo2.clear();
          fbdoStatus.clear();

          // FIX: Reset timers so Firebase waits 10 seconds to stabilize before sending data
          lastOnlineReport = millis(); 
          lastFirebaseCheck = millis(); 
      }

      // Only do Firebase stuff if WiFi is connected
      if (Firebase.ready() && (millis() - lastOnlineReport > 15000)) {
         lastOnlineReport = millis();
         DateTime now = rtc.now();
         FirebaseJson hb; 
         
         int standardUtcTime = now.unixtime() - 19800;
         hb.set("fields/lastSeen/integerValue", (int64_t)standardUtcTime); 
         
         Firebase.Firestore.patchDocument(&fbdoStatus, FIREBASE_PROJECT_ID, "", "status/esp32", hb.raw(), "lastSeen");
      }

      if (Firebase.ready()) {
          if (millis() - lastFirebaseCheck > 10000) { 
              lastFirebaseCheck = millis();
              
              checkCommands(cage1, fbdo1);
              checkCommands(cage2, fbdo2);
              
              managePowerSource(); 
          }
      }
  }

  // --- DATA COLLECTION ALWAYS RUNS ---
  collectData(cage1, 1);
  collectData(cage2, 2);
}

// -------------------- WEB LOGGING FUNCTION --------------------
void addWebLog(String msg) {
  Serial.println(msg); 
  webLogs[webLogIndex] = msg + "<br>"; 
  webLogIndex = (webLogIndex + 1) % MAX_WEB_LOGS;
}

// -------------------- RTC CALIBRATION LOGIC --------------------
void verifyRTC() {
  if (WiFi.status() == WL_CONNECTED) {
    addWebLog("Verifying RTC time against Internet...");
    configTime(19800, 0, "pool.ntp.org"); 
    
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) { 
      DateTime internetTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      DateTime rtcTime = rtc.now();

      uint32_t netUnix = internetTime.unixtime();
      uint32_t rtcUnix = rtcTime.unixtime();
      uint32_t diff = (netUnix > rtcUnix) ? (netUnix - rtcUnix) : (rtcUnix - netUnix);

      if (diff > 60 || rtcTime.year() < 2024) {
        rtc.adjust(internetTime);
        addWebLog("-> RTC was incorrect. Updated to Internet Time!");
      } else {
        addWebLog("-> RTC is perfectly accurate. No update needed.");
      }
    } else {
      addWebLog("-> Failed to reach NTP server. Relying on offline RTC battery.");
    }
  }
}

// -------------------- WEB SERVER (LOCAL HOST) --------------------
void handleRoot() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>ESP32 Research Data</title>
    <style>
      body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #1e1e2e; color: #cdd6f4; text-align: center; margin: 0; padding: 20px; }
      .container { max-width: 800px; margin: auto; background: #313244; padding: 20px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); }
      h2 { color: #a6e3a1; border-bottom: 2px solid #45475a; padding-bottom: 10px;}
      .file-card { background: #45475a; margin: 10px 0; padding: 15px; border-radius: 8px; display: flex; justify-content: space-between; align-items: center; text-align: left;}
      .file-info { flex-grow: 1; }
      .btn { padding: 8px 15px; border: none; border-radius: 5px; cursor: pointer; text-decoration: none; color: #11111b; font-weight: bold; transition: 0.3s; margin-left: 5px; font-size: 14px;}
      .btn-down { background: #89b4fa; }
      .btn-down:hover { background: #74c7ec; }
      .btn-del { background: #f38ba8; }
      .btn-del:hover { background: #eba0ac; }
      .terminal { background: #11111b; color: #a6e3a1; padding: 15px; border-radius: 8px; height: 250px; overflow-y: auto; text-align: left; font-family: monospace; margin-top: 20px; border: 1px solid #585b70; line-height: 1.5;}
    </style>
  </head>
  <body>
    <div class="container">
      <h2>🌱 Tomato Salinity Research Data</h2>
      <div id="file-list">
  )rawliteral";

  File root = SD.open("/");
  bool hasFiles = false;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break; 
    
    String fileName = String(entry.name());
    if (fileName.endsWith(".csv")) {
      hasFiles = true;
      html += "<div class='file-card'><div class='file-info'><b>" + fileName + "</b><br><small>" + String(entry.size() / 1024) + " KB</small></div>";
      html += "<div><a class='btn btn-down' href='/download?file=/" + fileName + "'>Download</a>";
      html += "<a class='btn btn-del' href='/delete?file=/" + fileName + "' onclick='return confirm(\"WARNING: Are you sure you want to delete " + fileName + "?\");'>Delete</a></div></div>";
    }
    entry.close();
  }
  
  if (!hasFiles) {
    html += "<p style='color: #f38ba8; padding: 20px;'>No CSV files found on SD Card.</p>";
  }

  html += R"rawliteral(
      </div>
      <h3 style="margin-top:40px; text-align:left; color:#89dceb; border-bottom: 2px solid #45475a; padding-bottom: 5px;">💻 Live Web Serial Monitor</h3>
      <div class="terminal" id="monitor">Waiting for system logs...</div>
    </div>
    
    <script>
      setInterval(() => {
        fetch('/logs').then(r => r.text()).then(text => {
          let mon = document.getElementById('monitor');
          mon.innerHTML = text;
          mon.scrollTop = mon.scrollHeight; 
        });
      }, 2000);
    </script>
  </body>
  </html>
  )rawliteral";
  
  server.send(200, "text/html", html);
}

void handleDownload() {
  if (server.hasArg("file")) {
    String fileName = server.arg("file");
    File file = SD.open(fileName, FILE_READ);
    if (!file) {
      server.send(404, "text/plain", "File not found!");
      return;
    }
    server.sendHeader("Content-Type", "text/csv");
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + fileName.substring(1) + "\"");
    server.sendHeader("Connection", "close");
    server.streamFile(file, "application/octet-stream");
    file.close();
    addWebLog("User downloaded: " + fileName);
  } else {
    server.send(400, "text/plain", "No file specified.");
  }
}

void handleDelete() {
  if (server.hasArg("file")) {
    String fileName = server.arg("file");
    if (SD.remove(fileName)) {
      addWebLog("DELETED file: " + fileName);
    } else {
      addWebLog("ERROR: Failed to delete " + fileName);
    }
    server.sendHeader("Location", "/");
    server.send(303);
  } else {
    server.send(400, "text/plain", "No file specified.");
  }
}

void handleLogs() {
  String logText = "";
  for (int i = 0; i < MAX_WEB_LOGS; i++) {
    int idx = (webLogIndex + i) % MAX_WEB_LOGS;
    if (webLogs[idx] != "") {
      logText += webLogs[idx];
    }
  }
  server.send(200, "text/plain", logText);
}

void checkCommands(CageState &cage, FirebaseData &fbdo) {
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", cage.configPath.c_str())) {
      FirebaseJson json; 
      json.setJsonData(fbdo.payload()); 
      FirebaseJsonData result;

      json.get(result, "fields/updatedAt/stringValue");
      String remoteUpdate = result.stringValue;

      if (remoteUpdate != "" && remoteUpdate != cage.lastUpdateID) {
        
        if (!cage.isBootSyncDone) {
            cage.lastUpdateID = remoteUpdate;
            cage.isBootSyncDone = true;
            return; 
        }

        json.get(result, "fields/duration/integerValue"); cage.durationMin = result.intValue; 
        json.get(result, "fields/timeGap/integerValue"); cage.timeGapSec = result.intValue; 
        json.get(result, "fields/currentVariety/stringValue"); cage.variety = result.stringValue;
        json.get(result, "fields/currentSalinity/stringValue"); cage.salinity = result.stringValue;
        json.get(result, "fields/targetBatch/stringValue"); cage.batch = result.stringValue;
        json.get(result, "fields/historyId/stringValue"); cage.historyId = result.stringValue;

        cage.lastUpdateID = remoteUpdate;
        
        if (cage.durationMin == 0) {
            cage.isRunning = false;
            addWebLog(cage.name + " STOPPED.");
        } 
        else {
            verifyRTC();
            cage.isRunning = true;
            cage.endTime = millis() + (cage.durationMin * 60000); 
            cage.lastSampleTime = millis(); 
            addWebLog(cage.name + " STARTING: " + cage.batch);
            
            if(cage.historyId != "") {
               FirebaseJson update; update.set("fields/status/stringValue", "Running");
               String path = "batch_history/" + cage.historyId;
               Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", path.c_str(), update.raw(), "status");
            }
        }
        
        FirebaseJson ack; ack.set("fields/lastCommand/stringValue", remoteUpdate);
        Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", "status/esp32", ack.raw(), "lastCommand");
      } 
      else if (!cage.isBootSyncDone) {
          cage.isBootSyncDone = true; 
      }
  }
}

void managePowerSource() {
  bool isCollectingData = cage1.isRunning || cage2.isRunning;

  if (isCollectingData && isAdapterPowerOn) {
    addWebLog("Switching to Battery Power for clean data reading...");
    digitalWrite(POWER_SWITCH_PIN, LOW); 
    isAdapterPowerOn = false;
    delay(100); 
  } 
  else if (!isCollectingData && !isAdapterPowerOn) {
    addWebLog("System Idle. Switching back to Adapter Power.");
    digitalWrite(POWER_SWITCH_PIN, HIGH); 
    isAdapterPowerOn = true;
    delay(100); 
  }
}

void collectData(CageState &cage, int cageNum) {
  if (!cage.isRunning) return; 

  if (millis() > cage.endTime) {
      cage.isRunning = false;
      addWebLog(cage.name + " FINISHED.");
      
      if (cage.historyId != "") {
         FirebaseJson update; update.set("fields/status/stringValue", "Completed");
         String path = "batch_history/" + cage.historyId;
         FirebaseData tempFbdo;
         Firebase.Firestore.patchDocument(&tempFbdo, FIREBASE_PROJECT_ID, "", path.c_str(), update.raw(), "status");
      }
      return;
  }

  if (millis() - cage.lastSampleTime >= (cage.timeGapSec * 1000)) {
      cage.lastSampleTime = millis();
      
      DateTime now = rtc.now();
      String dateToday = String(now.year()) + "-" + String(now.month()) + "-" + String(now.day());
      String timeNow = String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());

      float t = 0, h = 0, lux = 0;
      int rawElec = 0, rawSoil = 0;

      if (cageNum == 1) {
          t = dht1.readTemperature();
          h = dht1.readHumidity();
          lux = readLightAverage(lightMeter1);
          rawElec = readADS_Average(0, 5);
          rawSoil = readADS_Average(2, 5);
      } else {
          t = dht2.readTemperature();
          h = dht2.readHumidity();
          lux = readLightAverage(lightMeter2);
          rawElec = readADS_Average(1, 5);
          rawSoil = readADS_Average(3, 5);
      }

      // Convert raw ADS values to Millivolts (mV)
      float elecMV = (rawElec * 4096.0) / 32768.0;
      float soilMV = (rawSoil * 4096.0) / 32768.0;

      if (isnan(t)) t = 0.0;
      if (isnan(h)) h = 0.0;

      logToSDBatch(cage, dateToday, timeNow, t, h, lux, elecMV, soilMV);
  }
}

void logToSDBatch(CageState &cage, String dateStr, String timeStr, float t, float h, float lux, float elecMV, float soilMV) {
  String filename = "/" + cage.name + "_" + cage.batch + ".csv";
  filename.replace(" ", ""); 

  bool isNewFile = !SD.exists(filename);
  File file = SD.open(filename, FILE_APPEND);
  
  if (file) {
    if (isNewFile) {
      file.println("Date,Time,Variety,Salinity,Temp(C),Hum(%),Light(lux),Electrode(mV),SoilMoisture(mV)");
    }

    String dataRow = dateStr + "," + timeStr + "," + cage.variety + "," + cage.salinity + "," + 
                     String(t) + "," + String(h) + "," + String(lux) + "," + String(elecMV, 0) + "," + String(soilMV, 0);
    
    file.println(dataRow);
    file.close();
    
    String debugMsg = "[" + cage.name + " " + timeStr + "] T:" + String(t, 1) + "C | L:" + String(lux, 0) + "lx | E:" + String(elecMV, 0) + "mV | S:" + String(soilMV, 0) + "mV";

    addWebLog(debugMsg);

  } else {
    addWebLog("ERROR: Could not write to SD Card!");
  }
}

int readADS_Average(uint8_t channel, int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += ads.readADC_SingleEnded(channel);
    delay(10); 
  }
  return sum / samples;
}

float readLightAverage(BH1750 &sensor) {
  float sum = 0;
  for (int i = 0; i < 5; i++) { 
    sum += sensor.readLightLevel();
    delay(50); 
  }
  return sum / 5.0;
}