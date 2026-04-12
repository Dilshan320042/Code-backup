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
#include <ESPmDNS.h> // For http://tomato.local
#include <WebServer.h>
#include <Firebase_ESP_Client.h>
#include <time.h> 
#include <vector>
#include <algorithm>

// Provide the token generation process info
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- 1. FIREBASE & NETWORK CONFIGURATION ---
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
  String currentFileName; 
};

CageState cage1 = {"Cage1", "settings/cage1", false, 0, 0, "", 0, 0, "", "", "", "", false, ""};
CageState cage2 = {"Cage2", "settings/cage2", false, 0, 0, "", 0, 0, "", "", "", "", false, ""};

unsigned long lastOnlineReport = 0;
unsigned long lastWiFiCheck = 0; 
unsigned long lastFirebaseCheck = 0; 
bool isAdapterPowerOn = true; 

// --- FUNCTION PROTOTYPES ---
void checkCommands(CageState &cage, FirebaseData &fbdo);
void managePowerSource();
void collectData(CageState &cage, int cageNum);
void logToSDBatch(CageState &cage, String timeStr, float t, float h, float lux, float elecMV, float soilMV);
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

  pinMode(POWER_SWITCH_PIN, OUTPUT);
  digitalWrite(POWER_SWITCH_PIN, HIGH); 

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!rtc.begin()) addWebLog("RTC not found!");

  ledcAttach(MAX7400_CLK_PIN, 300, 8); 
  ledcWrite(MAX7400_CLK_PIN, 127); 

  dht1.begin();
  dht2.begin();
  if (ads.begin()) ads.setGain(GAIN_ONE);
  lightMeter1.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire); 
  lightMeter2.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x5C, &Wire);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    addWebLog("SD Card Mount Failed!");
  } else {
    addWebLog("SD Card Initialized!");
  }

  addWebLog("Connecting to Phone Hotspot...");
  WiFi.begin(ssid, password);
  
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    addWebLog("Connected!");
    addWebLog("IP: " + WiFi.localIP().toString());
    
    if (MDNS.begin("tomato")) {
      addWebLog("Web address: http://tomato.local");
    }
    
    verifyRTC(); 
  } else {
    addWebLog("WiFi Failed. OFFLINE MODE.");
  }

  server.on("/", handleRoot);
  server.on("/download", handleDownload);
  server.on("/delete", handleDelete);
  server.on("/logs", handleLogs);
  server.begin();

  fbdo1.setBSSLBufferSize(4096, 1024);
  fbdo2.setBSSLBufferSize(4096, 1024);
  fbdoStatus.setBSSLBufferSize(4096, 1024);

  config.api_key = API_KEY;
  config.signer.test_mode = true;
  config.timeout.socketConnection = 10000; 
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(false); 

  addWebLog("System Ready.");
}

// -------------------- MAIN LOOP --------------------
void loop() {
  // 1. HIGHEST PRIORITY: ALWAYS COLLECT DATA FIRST
  collectData(cage1, 1);
  collectData(cage2, 2);

  // 2. LOCAL HOST WEB SERVER
  server.handleClient(); 

  static bool wasOffline = false;

  // 3. BACKGROUND NETWORK TASKS
  if (WiFi.status() != WL_CONNECTED) {
      if (!wasOffline) {
          wasOffline = true;
          fbdo1.clear();
          fbdo2.clear();
          fbdoStatus.clear();
          addWebLog("WiFi lost. System running in Offline Mode...");
      }

      if (millis() - lastWiFiCheck > 30000) { 
          lastWiFiCheck = millis();
          WiFi.disconnect(); 
          WiFi.begin(ssid, password);
      }
  } else {
      if (wasOffline) {
          wasOffline = false;
          addWebLog("WiFi RECONNECTED! IP: " + WiFi.localIP().toString());
          
          MDNS.end();
          if (MDNS.begin("tomato")) {
              addWebLog("Web address: http://tomato.local restored");
          }
          server.begin(); 
          
          Firebase.begin(&config, &auth);
          
          lastOnlineReport = millis() + 5000;
          lastFirebaseCheck = millis() + 5000;
      }

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
}

void addWebLog(String msg) {
  Serial.println(msg); 
  webLogs[webLogIndex] = msg + "<br>"; 
  webLogIndex = (webLogIndex + 1) % MAX_WEB_LOGS;
}

void verifyRTC() {
  if (WiFi.status() == WL_CONNECTED) {
    configTime(19800, 0, "pool.ntp.org"); 
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) { 
      DateTime internetTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      rtc.adjust(internetTime);
    }
  }
}

void handleRoot() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>ESP32 Research Data</title>
    <style>
      body { font-family: sans-serif; background: #1e1e2e; color: #cdd6f4; text-align: center; padding: 20px; }
      .container { max-width: 800px; margin: auto; background: #313244; padding: 20px; border-radius: 12px; }
      h2 { color: #a6e3a1; }
      .file-card { background: #45475a; margin: 10px 0; padding: 15px; border-radius: 8px; display: flex; justify-content: space-between; align-items: center; text-align: left; }
      .btn { padding: 8px 15px; border-radius: 5px; text-decoration: none; color: #11111b; font-weight: bold; }
      .btn-down { background: #89b4fa; }
      .btn-del { background: #f38ba8; margin-left: 5px; }
      .terminal { background: #11111b; color: #a6e3a1; padding: 15px; border-radius: 8px; height: 200px; overflow-y: auto; text-align: left; font-family: monospace; margin-top: 20px; }
    </style>
  </head>
  <body>
    <div class="container">
      <h2>🌱 Tomato Salinity Research Data</h2>
      <div id="file-list">
  )rawliteral";

  std::vector<String> files;
  File root = SD.open("/");
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String fileName = String(entry.name());
    if (fileName.endsWith(".csv")) files.push_back(fileName);
    entry.close();
  }
  std::sort(files.begin(), files.end(), std::greater<String>());

  for (const String& fileName : files) {
    File entry = SD.open("/" + fileName);
    html += "<div class='file-card'><div><b>" + fileName + "</b><br><small>" + String(entry.size() / 1024) + " KB</small></div>";
    html += "<div><a class='btn btn-down' href='/download?file=/" + fileName + "'>Download</a>";
    html += "<a class='btn btn-del' href='/delete?file=/" + fileName + "' onclick='return confirm(\"Delete file?\");'>Delete</a></div></div>";
    entry.close();
  }

  html += R"rawliteral(
      </div>
      <div class="terminal" id="monitor">Loading logs...</div>
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

// --- FIXED DOWNLOD FUNCTION ---
void handleDownload() {
  if (server.hasArg("file")) {
    String fileName = server.arg("file");
    File file = SD.open(fileName, FILE_READ);
    
    if (!file) {
      server.send(404, "text/plain", "File not found!");
      return;
    }

    // These exact headers fix the incomplete download issue
    server.sendHeader("Content-Type", "text/csv");
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + fileName.substring(1) + "\"");
    server.sendHeader("Content-Length", String(file.size())); 
    server.sendHeader("Connection", "keep-alive");

    server.streamFile(file, "application/octet-stream");
    file.close();
    
    addWebLog("Downloaded: " + fileName);
  } else {
    server.send(400, "text/plain", "No file specified.");
  }
}

void handleDelete() {
  if (server.hasArg("file")) {
    SD.remove(server.arg("file"));
    server.sendHeader("Location", "/");
    server.send(303);
  }
}

void handleLogs() {
  String logText = "";
  for (int i = 0; i < MAX_WEB_LOGS; i++) {
    int idx = (webLogIndex + i) % MAX_WEB_LOGS;
    if (webLogs[idx] != "") logText += webLogs[idx];
  }
  server.send(200, "text/plain", logText);
}

void checkCommands(CageState &cage, FirebaseData &fbdo) {
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", cage.configPath.c_str())) {
      FirebaseJson json; json.setJsonData(fbdo.payload()); FirebaseJsonData result;
      json.get(result, "fields/updatedAt/stringValue");
      String remoteUpdate = result.stringValue;

      if (remoteUpdate != "" && remoteUpdate != cage.lastUpdateID) {
        if (!cage.isBootSyncDone) { cage.lastUpdateID = remoteUpdate; cage.isBootSyncDone = true; return; }
        json.get(result, "fields/duration/integerValue"); cage.durationMin = result.intValue; 
        json.get(result, "fields/timeGap/integerValue"); cage.timeGapSec = result.intValue; 
        json.get(result, "fields/currentVariety/stringValue"); cage.variety = result.stringValue;
        json.get(result, "fields/currentSalinity/stringValue"); cage.salinity = result.stringValue;
        json.get(result, "fields/targetBatch/stringValue"); cage.batch = result.stringValue;
        json.get(result, "fields/historyId/stringValue"); cage.historyId = result.stringValue;
        cage.lastUpdateID = remoteUpdate;
        
        if (cage.durationMin == 0) cage.isRunning = false;
        else {
            verifyRTC(); 
            cage.isRunning = true;
            cage.endTime = millis() + (cage.durationMin * 60000); 
            cage.lastSampleTime = millis(); 

            DateTime now = rtc.now();
            String dateStr = String(now.year()) + "-" + (now.month() < 10 ? "0" : "") + String(now.month()) + "-" + (now.day() < 10 ? "0" : "") + String(now.day());
            String baseFilename = "/" + dateStr + "_" + cage.name + "_" + cage.variety + "_" + cage.salinity;
            baseFilename.replace(" ", ""); 

            int fileIndex = 1;
            String finalFilename = baseFilename + "_" + String(fileIndex) + ".csv";
            
            while (SD.exists(finalFilename)) {
                fileIndex++;
                finalFilename = baseFilename + "_" + String(fileIndex) + ".csv";
            }
            
            cage.currentFileName = finalFilename; 

            if(cage.historyId != "") {
               FirebaseJson update; update.set("fields/status/stringValue", "Running");
               Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", ("batch_history/" + cage.historyId).c_str(), update.raw(), "status");
            }
        }
        FirebaseJson ack; ack.set("fields/lastCommand/stringValue", remoteUpdate);
        Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", "status/esp32", ack.raw(), "lastCommand");
      } else if (!cage.isBootSyncDone) cage.isBootSyncDone = true;
  }
}

void managePowerSource() {
  bool isCollectingData = cage1.isRunning || cage2.isRunning;
  if (isCollectingData && isAdapterPowerOn) {
    digitalWrite(POWER_SWITCH_PIN, LOW); isAdapterPowerOn = false; delay(100); 
  } else if (!isCollectingData && !isAdapterPowerOn) {
    digitalWrite(POWER_SWITCH_PIN, HIGH); isAdapterPowerOn = true; delay(100); 
  }
}

void collectData(CageState &cage, int cageNum) {
  if (!cage.isRunning) return; 
  if (millis() > cage.endTime) {
      cage.isRunning = false;
      addWebLog("[" + cage.name + "] DATA COLLECTION OVER. Batch finished.");
      if (cage.historyId != "") {
         FirebaseJson update; update.set("fields/status/stringValue", "Completed");
         Firebase.Firestore.patchDocument(&fbdoStatus, FIREBASE_PROJECT_ID, "", ("batch_history/" + cage.historyId).c_str(), update.raw(), "status");
      }
      return;
  }

  if (millis() - cage.lastSampleTime >= (cage.timeGapSec * 1000)) {
      cage.lastSampleTime = millis();
      DateTime now = rtc.now();
      String timeNow = String(now.hour()) + ":" + (now.minute() < 10 ? "0" : "") + String(now.minute()) + ":" + (now.second() < 10 ? "0" : "") + String(now.second());

      float t = (cageNum == 1) ? dht1.readTemperature() : dht2.readTemperature();
      float h = (cageNum == 1) ? dht1.readHumidity() : dht2.readHumidity();
      float lux = (cageNum == 1) ? readLightAverage(lightMeter1) : readLightAverage(lightMeter2);
      int rawElec = (cageNum == 1) ? readADS_Average(0, 5) : readADS_Average(1, 5);
      int rawSoil = (cageNum == 1) ? readADS_Average(2, 5) : readADS_Average(3, 5);

      logToSDBatch(cage, timeNow, t, h, lux, (rawElec * 4096.0) / 32768.0, (rawSoil * 4096.0) / 32768.0);
  }
}

void logToSDBatch(CageState &cage, String timeStr, float t, float h, float lux, float elecMV, float soilMV) {
  if (cage.currentFileName == "") return; 

  bool isNewFile = !SD.exists(cage.currentFileName);
  File file = SD.open(cage.currentFileName, FILE_APPEND);
  
  if (file) {
    if (isNewFile) {
      file.println("Time,Variety,Salinity,Temp(C),Hum(%),Light(lux),Electrode(mV),SoilMoisture(mV)");
    }
    String dataRow = timeStr + "," + cage.variety + "," + cage.salinity + "," + 
                     String(t) + "," + String(h) + "," + String(lux) + "," + String(elecMV, 0) + "," + String(soilMV, 0);
    file.println(dataRow);
    file.close();
    
    String debugMsg = "[" + cage.name + " " + timeStr + "] sali:" + cage.salinity + " | tem:" + String(t, 1) + " | hum:" + String(h, 1) + " | lux:" + String(lux, 0) + " | ele:" + String(elecMV, 0) + " | soil:" + String(soilMV, 0);
    addWebLog(debugMsg);

  } else {
    addWebLog("ERROR: Could not write to SD Card!");
  }
}

int readADS_Average(uint8_t channel, int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) { sum += ads.readADC_SingleEnded(channel); delay(10); }
  return sum / samples;
}

float readLightAverage(BH1750 &sensor) {
  float sum = 0;
  for (int i = 0; i < 5; i++) { sum += sensor.readLightLevel(); delay(50); }
  return sum / 5.0;
}