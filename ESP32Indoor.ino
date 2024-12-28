#include <SD.h> // https://docs.arduino.cc/libraries/sd/#SD%20class
#include <WiFi.h>
#include <time.h>
#include <ESPmDNS.h>
#include <ESPping.h>
#include <SimpleDHT.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>

// ================================================== Definitions ================================================== //
//#define ENABLE_SERIAL_LOGGER  // Use this when debugging
//#define ENABLE_SD_LOGGING

#define MAX_WIFI_TRYS 5

#define DNS_ADDRESS "indoor"  // DNS_ADDRESS.local

#define WEBSERVER_PORT 80 // Default IP for web server when are running Access Point mode is 192.168.4.1

#define MAX_SOIL_READS 10  // Max 255, cuz is uint8_t
#define MAX_ENVIRONMENT_READS 5

#define HW080_MIN 0     // https://cms.katranji.com/web/content/723081
#define HW080_MAX 2800  // I'm using 20k resistors, so the max value never go up to 4095

#define TOTAL_SOIL_HUMIDITY_SENSORS 2

#define S8050_FREQUENCY 300   // https://www.mouser.com/datasheet/2/149/SS8050-117753.pdf
#define S8050_RESOLUTION 12
#define S8050_MAX_VALUE 4095  // cuz is 12 bits of resolution

#define DHT_DATA_PIN 16 // https://www.mouser.com/datasheet/2/758/DHT11-Technical-Data-Sheet-Translated-Version-1143054.pdf

#define SD_CS_PIN 5

#define RELAY_0_PIN 14  // Lights
#define RELAY_1_PIN 27  // Internal Fan
#define RELAY_2_PIN 26  // Ventilation
#define RELAY_3_PIN 25  // Water Pump

#define S8050_PWM_PIN 33  // I'm using a 1K resistor in serie in BASE pin

#define SOIL_SENSORS_VCC_PIN 32

uint8_t uSoilsHumidityPins[TOTAL_SOIL_HUMIDITY_SENSORS] = {
  34,  // Soil Humidity Sensor 0
  35   // Soil Humidity Sensor 1
};

enum ERR_TYPE {
  INFO,
  WARN,
  ERROR
};

String strSoilGraphColor[] = {
    "#B57165",
    "#784B43"/*,
    "#8A5B50",
    "#9C6D5A"*/ // Add more colors for graph here
};

// ================================================== Globals ================================================== //
// =============== Loaded from Settings =============== //
String strSSID, strSSIDPWD;

uint8_t uStartLightTime = 0;
uint8_t uStopLightTime = 0;
uint16_t uLightBrightness = 0;

uint8_t uStartFanTemperature = 0;

uint8_t uStartVentilationTemperature = 0;
uint8_t uStartVentilationHumidity = 0;

uint8_t uWateringMode = 0;
uint64_t uWateringInterval = 0;
uint8_t uStartWateringHumidity = 0;
uint16_t uWateringTime = 0;

uint32_t uFansRestIntervalTime = 0;
uint32_t uFansRestDuration = 0;

uint32_t uSoilReadsInterval = 0;

uint8_t uTemperatureStopHysteresis = 0;
uint8_t uHumidityStopHysteresis = 0;

uint16_t uDripPerMinute = 0;

// =============== Time & Sync =============== //
unsigned long lStartupTime = 0;
unsigned long lEnvironmentElapsedReadTime = 0;
unsigned long lSoilHumidityElapsedReadTime = 0;
unsigned long lWateringIntervalElapsedTime = 0;
unsigned long lEachSecondMillis = 0;
unsigned long lEachMinuteMillis = 0;
uint16_t uWateringElapsedTime = 0;
bool bVentilationByTemperature = false;
bool bVentilationByHumidity = false;
unsigned long lFansRestIntervalTime = 0;
unsigned long lFansRestElapsedTime = 0;
bool bFansRest = false;
uint8_t uEffectiveStartLights = 0;
uint8_t uEffectiveStopLights = 0;
TaskHandle_t taskhandleWiFiReconnect;
unsigned long lLastStoreElapsedTime = 0;
bool bSDInit = false;
bool bConnectedToInternet = false;
size_t sizeLastReadFile = 0;
const char* strArrayGraphData[48] = {};

// =============== Parameters from Indoor =============== //
uint8_t nEnvironmentTemperature = 0;
uint8_t nEnvironmentHumidity = 0;
float fEnvironmentVPD = 0.0f;
uint8_t uSoilsHumidity[TOTAL_SOIL_HUMIDITY_SENSORS] = {};

// ================================================== Instances ================================================== //
AsyncWebServer AsyncWebServerHandle(WEBSERVER_PORT);

SimpleDHT11 DHT11(DHT_DATA_PIN);

Preferences pSettings;

// ================================================== Helper Functions ================================================== //
void WriteToSD(String strFileName, String strText, bool bAppend) {
  ConnectSD(false);

  if (bSDInit) {
    if (strFileName == "logging_") {
      time_t now = time(nullptr);
      struct tm *timeInfo = localtime(&now);

      strFileName += String(timeInfo->tm_year + 1900) + "_" + String(timeInfo->tm_mon + 1) + "_" + String(timeInfo->tm_mday) + ".log";
    }

    File pFile = SD.open(strFileName.c_str(), (bAppend ? FILE_APPEND : FILE_WRITE));
    if (pFile) {
      pFile.println(strText.c_str());
      pFile.close();
    }
  }
}

void LOGGER(const char *format, ERR_TYPE nType, ...) {
#if defined(ENABLE_SD_LOGGING) || defined(ENABLE_SERIAL_LOGGER)
  va_list args;

  char cPrefix[11];
  char cBuffer[1024];

  switch (nType) {
    case INFO:
      snprintf(cPrefix, sizeof(cPrefix), "[INFO] ");
      break;
    case WARN:
      snprintf(cPrefix, sizeof(cPrefix), "[WARNING] ");
      break;
    case ERROR:
      snprintf(cPrefix, sizeof(cPrefix), "[ERROR] ");
      break;
    default:
      snprintf(cPrefix, sizeof(cPrefix), "[UNKNOWN] ");
      break;
  }

  va_start(args, nType);

  snprintf(cBuffer, sizeof(cBuffer), "%s", cPrefix);
  vsnprintf(cBuffer + strlen(cPrefix), sizeof(cBuffer) - strlen(cPrefix), format, args);

  va_end(args);

#if defined(ENABLE_SD_LOGGING)
  WriteToSD("/logging_", String(cBuffer), true);
#elif defined(ENABLE_SERIAL_LOGGER)
  Serial.println(cBuffer);
#endif
#else
  return;
#endif
}

void GetEnvironmentParameters(uint8_t &nTemperature, uint8_t &nHumidity, float &fVPD) {
  int nError = SimpleDHTErrSuccess;
  uint8_t uReadTrysCount = 0;
  byte bTemperature = 0, bHumidity = 0;

  do {  // Try read Temp & Humidity values from Environment
    if ((nError = DHT11.read(&bTemperature, &bHumidity, NULL)) != SimpleDHTErrSuccess) {  // Get Environment Temperature and Humidity
      uReadTrysCount++;

      LOGGER("Read DHT11 failed, Error=%d, Duration=%d.", ERROR, SimpleDHTErrCode(nError), SimpleDHTErrDuration(nError));

      delay(100);
    }
  } while (nError != SimpleDHTErrSuccess && uReadTrysCount < MAX_ENVIRONMENT_READS);  // Repeat while it fails and not reach max trys

  if (nError == SimpleDHTErrSuccess) {
    nTemperature = (uint8_t)bTemperature;
    nHumidity = (uint8_t)bHumidity;

    if ((nTemperature > 0 && nTemperature <= 52) && (nHumidity > 0 && nHumidity <= 100)) {  // DHT11 → Temp 0~50±2 | Hum 0~80±5
      float E_s = 6.112 * exp((17.67 * nTemperature) / (243.5 + nTemperature));

      fVPD = (E_s - (nHumidity / 100.0) * E_s) / 10.0;
    } else {
      fVPD = 0;

      LOGGER("Invalid Temperature or Humidity readings.", ERROR);
    }
  } else {
    nTemperature = 0;
    nHumidity = 0;
    fVPD = 0;

    LOGGER("Failed to read DHT11 after max retries.", ERROR);
  }
}

uint8_t GetSoilHumidity(uint8_t nSensorNumber) {
  digitalWrite(SOIL_SENSORS_VCC_PIN, HIGH);  // Put pin output pin in high to excite the moisture sensors

  delay(10);  // Wait for a stable reading

  uint32_t uCombinedValues = 0;

  for (uint8_t i = 0; i < MAX_SOIL_READS; i++) {
    uCombinedValues += analogRead(uSoilsHumidityPins[nSensorNumber]);  // Sumar cada lectura

    delay(100);  // Small wait between reads
  }

  digitalWrite(SOIL_SENSORS_VCC_PIN, LOW);  // Put pin output in low to stop electrolysis

  return constrain(map(uCombinedValues / MAX_SOIL_READS, HW080_MIN, HW080_MAX, 0, 100), 0, 100);
}

String HTMLProcessor(const String &var) {
  if (var == "DPM") {
    return String(uDripPerMinute);
  } else if (var == "ENVTEMP") {
    return String(nEnvironmentTemperature);
  } else if (var == "ENVHUM") {
    return String(nEnvironmentHumidity);
  } else if (var == "VPDSECTION") {
    String strHTMLColor = "#FE7F96", strState = "Zona de Peligro";

    if (fEnvironmentVPD >= 0.4 && fEnvironmentVPD <= 0.8) {  // Propagation / Early Veg
      strHTMLColor = "#6497C9";
      strState = "Propagacíon/Inicio del Vegetativo";
    } else if (fEnvironmentVPD > 0.8 && fEnvironmentVPD <= 1.2) {  // Late Veg / Early Flower
      strHTMLColor = "#7FC794";
      strState = "Vegetativo/Inicio de Floración";
    } else if (fEnvironmentVPD > 1.2 && fEnvironmentVPD <= 1.6) {  // Mid / Late Flower
      strHTMLColor = "#F9AE54";
      strState = "Floración";
    }

    return "<tr><td>Déficit de Presión de Vapor:</td><td><font id=vpd color=" + strHTMLColor + ">" + String(fEnvironmentVPD, 2) + "</font>kPa</td><td>(<font id=vpdstate color=" + strHTMLColor + ">" + strState + "</font>)</td></tr>";
  } else if (var == "SOILSECTION") {
    String strReturn;

    for (uint8_t i = 0; i < TOTAL_SOIL_HUMIDITY_SENSORS; i++) {
      strReturn += "<tr><td>Humedad de Maceta " + String(i) + ":</td><td><p id=soil" + String(i) + ">" + String(uSoilsHumidity[i]) + "&#37;</p></td>";

      if (i == 0)
        strReturn += "<td rowspan=" + String(TOTAL_SOIL_HUMIDITY_SENSORS) + " id=wateringstate style=color:#72D6EB>" + (uWateringElapsedTime > 0 ? "Regando...<br>Tiempo Transcurrido: " + String(uWateringElapsedTime) + " segundos" : "") + "</td>";

      strReturn += "</tr>";
    }

    return strReturn;
  } else if (var == "CURRENTTIME") {
    time_t now = time(nullptr);
    struct tm *timeInfo = localtime(&now);
    auto timeformat = [](int value) {
      return String(value).length() == 1 ? "0" + String(value) : String(value);
    };

    return String(timeformat(timeInfo->tm_hour) + ":" + timeformat(timeInfo->tm_min) + ":" + timeformat(timeInfo->tm_sec));
  } else if (var == "STARTLIGHT") {
    return String(uStartLightTime);
  } else if (var == "STOPLIGHT") {
    return String(uStopLightTime);
  } else if (var == "MAXBRIGHT") {
    return String(S8050_MAX_VALUE);
  } else if (var == "BRIGHTLEVEL") {
    return String(uLightBrightness < 401 ? 0 : uLightBrightness);
  } else if (var == "STAINTFAN") {
    return String(uStartFanTemperature);
  } else if (var == "TEMPSTARTVENT") {
    return String(uStartVentilationTemperature);
  } else if (var == "HUMSTARTVENT") {
    return String(uStartVentilationHumidity);
  } else if (var == "RESTSTATE") {
    String strReturn;

    if (lFansRestElapsedTime > 0) {
      strReturn = "En Reposo...<br>Tiempo Restante: ";

      unsigned long lTimeRemaining = (uFansRestDuration * 60000) - (millis() - lFansRestElapsedTime);

      if (lTimeRemaining < 60000) {
        strReturn += String(lTimeRemaining / 1000) + " segundos";
      } else {
        unsigned long lMinutes = lTimeRemaining / 60000;
        unsigned long lSeconds = (lTimeRemaining % 60000) / 1000;

        strReturn += String(lMinutes) + (lMinutes == 1 ? " minuto" : " minutos");

        if (lSeconds > 0)
          strReturn += " y " + String(lSeconds) + " segundos";
      }
    }

    return strReturn;
  } else if (var == "WATERINGMODE") {
    String strReturn;

    if (uWateringMode == 1)
      strReturn = "checked";

    return strReturn;
  } else if (var == "STAWATHUM") {
    if (uWateringMode == 0)
      return String(uStartWateringHumidity);
    else
      return String(uWateringInterval / 3600000);
  } else if (var == "WATERINGTIME") {
    return String(uWateringTime);
  } else if (var == "RESTINTERVAL") {
    return String(uFansRestIntervalTime);
  } else if (var == "RESTDUR") {
    return String(uFansRestDuration);
  } else if (var == "SOILDREADINT") {
    return String(uSoilReadsInterval / 1000);
  } else if (var == "TEMPSTOPHYS") {
    return String(uTemperatureStopHysteresis);
  } else if (var == "HUMSTOPHYS") {
    return String(uHumidityStopHysteresis);
  } else if (var == "SSID") {
    return String(strSSID);
  } else if (var == "SSIDPWD") {
    return String(strSSIDPWD);
  } else if (var == "SOILLINES") {
    String strReturn;

    for (uint8_t i = 0; i < TOTAL_SOIL_HUMIDITY_SENSORS; i++)
      strReturn += ",{label:'Humedad de Maceta " + String(i) + "',borderColor:'" + strSoilGraphColor[i] + "',backgroundColor:'" + strSoilGraphColor[i] + "',symbol:'%%',yAxisID:'" + (3 + i) + "'}";

    return strReturn;
  }

  return String();
}

void ConnectSD(bool bShowSuccessLog) {
  if (!SD.open("/")) {
    SD.end();

    bSDInit = SD.begin(SD_CS_PIN);

    if (!bSDInit)
      LOGGER("Failed to initialize SD Card File System.", ERROR);

    if (bShowSuccessLog)
      LOGGER("SD Card File System initialized.", INFO);
  } else {  // Just in case
    bSDInit = true;
  }
}

bool CheckInternetConnection() {
  const char* cIP = "pool.ntp.org";

  LOGGER("Pinging to: %s...", INFO, String(cIP));

  if (Ping.ping(cIP, 1)) {
    bConnectedToInternet = true;
  } else {
    LOGGER("Ping to: %s failed.", ERROR, String(cIP));

    bConnectedToInternet = false;
  }

  return bConnectedToInternet;
}

void GetDateTime(bool bDoPing) {
  time_t now = 0;

  if (bDoPing)
    CheckInternetConnection();

  if (bConnectedToInternet) {
    LOGGER("Getting Datetime from NTP Server...", INFO);

    configTime(-3 * 3600, 0, "pool.ntp.org");

    now = time(nullptr);
  }

  if (now == 0 && bSDInit) {
    if (SD.exists("/time")) {
      File pFile = SD.open("/time", FILE_READ);
      if (pFile) {
        LOGGER("Getting Datetime from SD Card...", INFO);

        struct timeval tv;
        tv.tv_sec = pFile.parseInt();
        tv.tv_usec = 0;

        settimeofday(&tv, nullptr);

        configTime(-3 * 3600, 0, nullptr, nullptr);

        time_t now = time(nullptr);
        struct tm timeinfo;

        localtime_r(&now, &timeinfo);

        pFile.close();
      }
    }
  }

  if (struct tm *timeInfo = localtime(&now))
    LOGGER("Current Datetime: %04d-%02d-%02d %02d:%02d:%02d.", INFO, timeInfo->tm_year + 1900, timeInfo->tm_mon + 1, timeInfo->tm_mday, timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);
  else
    LOGGER("Failed to get Datetime.", ERROR);
}

void Thread_WifiReconnect(void *parameter) {
  LOGGER("Starting Access Point (SSID: ESP32_Indoor) mode for reconfiguration.", INFO);

  WiFi.mode(WIFI_AP_STA);  // Set both AP and STA modes

  vTaskDelay(100 / portTICK_PERIOD_MS);  // Delay to stabilize AP

  WiFi.softAP("ESP32_Indoor");  // Start Access Point, while try to connect to wifi

  GetDateTime(false);

  vTaskDelay(500 / portTICK_PERIOD_MS);  // Delay to stabilize AP

  LOGGER("Trying to reconnect WiFi...", INFO);

  WiFi.begin(strSSID, strSSIDPWD);  // Try connect to new wifi

  while (WiFi.status() != WL_CONNECTED)
    vTaskDelay(1000 / portTICK_PERIOD_MS);  // Wait 1 second before try again

  LOGGER("Connected to WiFi SSID: %s PASSWORD: %s. IP: %s.", INFO, strSSID, strSSIDPWD, WiFi.localIP().toString().c_str());

  WiFi.mode(WIFI_STA);
  WiFi.softAPdisconnect(true);

  LOGGER("Access Point disconnected.", INFO);

  GetDateTime(true);

  vTaskSuspend(taskhandleWiFiReconnect);  // Suspends the task until needed again
}

// ================================================== Main Functions ================================================== //
void setup() {
#ifdef ENABLE_SERIAL_LOGGER
  Serial.begin(9600);
  delay(3000);
#endif
  LOGGER("========== Indoor Controller Started ==========", INFO);

  lStartupTime = millis();

  LOGGER("Initializing PINS...", INFO);

  pinMode(RELAY_0_PIN, OUTPUT);
  digitalWrite(RELAY_0_PIN, HIGH);

  pinMode(RELAY_1_PIN, OUTPUT);
  digitalWrite(RELAY_1_PIN, HIGH);

  pinMode(RELAY_2_PIN, OUTPUT);
  digitalWrite(RELAY_2_PIN, HIGH);

  pinMode(RELAY_3_PIN, OUTPUT);
  digitalWrite(RELAY_3_PIN, HIGH);

  ledcAttach(S8050_PWM_PIN, S8050_FREQUENCY, S8050_RESOLUTION);

  pinMode(SOIL_SENSORS_VCC_PIN, OUTPUT);
  digitalWrite(SOIL_SENSORS_VCC_PIN, LOW);

  for (uint8_t i = 0; i < TOTAL_SOIL_HUMIDITY_SENSORS; i++)
    pinMode(uSoilsHumidityPins[i], INPUT);

  if (!pSettings.begin("Settings", false))
    LOGGER("Failed to initialize Settings.", ERROR);

  LOGGER("Settings System initialized.", INFO);

  LOGGER("Loading Settings...", INFO);

  strSSID = pSettings.getString("SSID", "TODO");
  strSSIDPWD = pSettings.getString("SSIDPWD", "TODO");

  uStartLightTime = pSettings.getUChar("StartLightTime", 6);
  uStopLightTime = pSettings.getUChar("StopLightTime", 24);
  uLightBrightness = pSettings.getUShort("LightBright", S8050_MAX_VALUE);  // 0 = 100% brightness cuz is NPN

  uStartFanTemperature = pSettings.getUChar("StartFanTemp", 27);

  uStartVentilationTemperature = pSettings.getUChar("StartVentTemp", 30);
  uStartVentilationHumidity = pSettings.getUChar("StartVentHumi", 65); // VEG: 60 ~ 70 BLOOM: 40 ~ 50

  uWateringMode = pSettings.getUChar("WateringMode", 0);
  uWateringInterval = pSettings.getULong64("WateringInt", 0);
  uStartWateringHumidity = pSettings.getUChar("StartWtrHum", 0);
  uWateringTime = pSettings.getUShort("WateringTime", 60);

  uFansRestIntervalTime = pSettings.getUInt("FanRestIntTime", 60);
  uFansRestDuration = pSettings.getUInt("FanRestDur", 5);

  uSoilReadsInterval = pSettings.getUInt("SoilReadsInt", 60000);

  uTemperatureStopHysteresis = pSettings.getUChar("TempHysteresis", 3);
  uHumidityStopHysteresis = pSettings.getUChar("HumHysteresis", 10);

  uDripPerMinute = pSettings.getUShort("DripPerMinute", 25);

  uEffectiveStartLights = (uStartLightTime == 24) ? 0 : uStartLightTime;  // Si la Hora de encendido definida es 24 convertirla a 0 (Medianoche)
  uEffectiveStopLights = (uStopLightTime == 0) ? 24 : uStopLightTime;     // Si la Hora de apagado definida es 24 convertirla a 0 (Medianoche)

  ConnectSD(true);

  LOGGER("Initializing WiFi...", INFO);

  WiFi.begin(strSSID, strSSIDPWD);

  uint8_t uConnectTrysCount = 0;

  while (WiFi.status() != WL_CONNECTED && uConnectTrysCount < MAX_WIFI_TRYS) {
    uConnectTrysCount++;

    delay(1000);  // Wait 1 second before try again
  }

  if (WiFi.status() == WL_CONNECTED)
    LOGGER("Connected to WiFi SSID: %s PASSWORD: %s. IP: %s.", INFO, strSSID, strSSIDPWD, WiFi.localIP().toString().c_str());
  else // Create an Access Point to reconfigure the SSID & PASSWORD
    LOGGER("Max WiFi reconnect attempts reached.", ERROR);

  GetDateTime(true);

  LOGGER("Initializing WiFi reconnect task thread...", INFO);
  xTaskCreatePinnedToCore(Thread_WifiReconnect, "WiFi Reconnect Task", 4096, NULL, 1, &taskhandleWiFiReconnect, 0);
  vTaskSuspend(taskhandleWiFiReconnect);  // Suspend it, its not necessary right now.

  LOGGER("Initializing mDNS...", INFO);
  if (!MDNS.begin(DNS_ADDRESS)) {
    LOGGER("Failed to initialize mDNS.", ERROR);
  } else {
    MDNS.addService("http", "tcp", WEBSERVER_PORT);

    LOGGER("mDNS Started at: %s.local Service: HTTP Protocol: TCP Port: %d.", INFO, DNS_ADDRESS, WEBSERVER_PORT);
  }

  LOGGER("Setting up Light Brightness...", INFO);
  ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - (uLightBrightness < 401 ? 0 : uLightBrightness));

  LOGGER("Setting up Web Server Paths & Commands...", INFO);

  const char* strFiles[] = {
    "fan.webp",
    "chart.js" // Add more files to server by web server here
  };

  for (uint8_t i = 0; i < sizeof(strFiles) / sizeof(strFiles[0]); i++) {
    String path = "/" + String(strFiles[i]);

    AsyncWebServerHandle.serveStatic(path.c_str(), SD, path.c_str()).setCacheControl("max-age=86400");
  }

  AsyncWebServerHandle.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    IPAddress clientIP = request->client()->getRemoteAddress();

    LOGGER("HTTP Request IP From: %d.%d.%d.%d.", INFO, clientIP[0], clientIP[1], clientIP[2], clientIP[3]);

    if (request->hasArg("action")) {
      if (request->arg("action") == "ask") {
        String strReturn = "UPD";

        if (request->hasArg("watering")) {
          strReturn += "wathum:";

          if (request->arg("watering").toInt() == 0)
            strReturn += String(uStartWateringHumidity);
          else
            strReturn += String(uWateringInterval / 3600000);
        }

        if (strReturn != "")
          request->send(200, "text/plain", strReturn);
      } else if (request->arg("action") == "update") {
        String strReturn;

        // =============== Start Light =============== //
        uint32_t uNewValue;

        if (request->hasArg("lightstart")) {
          uNewValue = request->arg("lightstart").toInt();

          if (uNewValue != uStartLightTime) {
            uStartLightTime = uNewValue;
            uEffectiveStartLights = (uStartLightTime == 24) ? 0 : uStartLightTime;  // Si la Hora de encendido definida es 24 convertirla a 0 (Medianoche)

            pSettings.putUChar("StartLightTime", uStartLightTime);

            strReturn += "\r\nSe actualizó la Hora de Encendido de Luz.";
          }
        }

        // =============== Stop Light =============== //
        if (request->hasArg("lightstop")) {
          uNewValue = request->arg("lightstop").toInt();

          if (uNewValue != uStopLightTime) {
            uStopLightTime = uNewValue;
            uEffectiveStopLights = (uStopLightTime == 0) ? 24 : uStopLightTime;  // Si la Hora de apagado definida es 24 convertirla a 0 (Medianoche)

            pSettings.putUChar("StopLightTime", uStopLightTime);

            strReturn += "\r\nSe actualizó la Hora de Apagado de Luz.";
          }
        }

        // =============== Light Brightness =============== //
        if (request->hasArg("lightbright")) {
          uNewValue = request->arg("lightbright").toInt();

          if (uNewValue != uLightBrightness) {
            uLightBrightness = uNewValue < 401 ? 0 : uNewValue;

            ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - uLightBrightness);

            pSettings.putUShort("LightBright", uLightBrightness);

            strReturn += "\r\nSe actualizó la Intensidad de Luz.";
          }
        }

        // =============== Internal Fan Start =============== //
        if (request->hasArg("intfansta")) {
          uNewValue = request->arg("intfansta").toInt();

          if (uNewValue != uStartFanTemperature) {
            uStartFanTemperature = uNewValue;

            pSettings.putUChar("StartFanTemp", uStartFanTemperature);

            strReturn += "\r\nSe actualizó la Temperatura de Encendido del Ventilador Interno.";
          }
        }

        // =============== Ventilation Temperature Start =============== //
        if (request->hasArg("venttempstart")) {
          uNewValue = request->arg("venttempstart").toInt();

          if (uNewValue != uStartVentilationTemperature) {
            uStartVentilationTemperature = uNewValue;

            pSettings.putUChar("StartVentTemp", uStartVentilationTemperature);

            strReturn += "\r\nSe actualizó la Temperatura de Encendido de Recirculación.";
          }
        }

        // =============== Ventilation Humidity Start =============== //
        if (request->hasArg("venthumstart")) {
          uNewValue = request->arg("venthumstart").toInt();

          if (uNewValue != uStartVentilationHumidity) {
            uStartVentilationHumidity = uNewValue;

            pSettings.putUChar("StartVentHumi", uStartVentilationHumidity);

            strReturn += "\r\nSe actualizó la Humedad de Encendido de la Recirculación.";
          }
        }

        // =============== Watering Mode =============== //
        if (request->hasArg("wateringmode")) {
          uNewValue = request->arg("wateringmode").toInt();

          if (uNewValue != uWateringMode) {
            uWateringMode = uNewValue;

            pSettings.putUChar("WateringMode", uWateringMode);

            strReturn += "\r\nSe actualizó el Método de Riego.";
          }
        }

        // =============== Soil Humidity for Start Watering / Watering Intervals =============== //
        if (request->hasArg("wathum")) {
          uNewValue = request->arg("wathum").toInt();

          if (uWateringMode == 0) { // Watering by Humidity
            if (uNewValue != uStartWateringHumidity) {
              uStartWateringHumidity = uNewValue;

              pSettings.putUChar("StartWtrHum", uStartWateringHumidity);

              strReturn += "\r\nSe actualizó el Nivel de Humedad mínimo para Regar.";
            }
          } else {  // Watering By Intervals
            uNewValue *= 3600000;

            if (uNewValue != uWateringInterval) {
              uWateringInterval = uNewValue;

              pSettings.putUChar("WateringInt", uWateringInterval);

              strReturn += "\r\nSe actualizó el Intervalo de Riego.";
            }
          }
        }

        // =============== Watering Time =============== //
        if (request->hasArg("wateringtime")) {
          uNewValue = request->arg("wateringtime").toInt();

          if (uNewValue != uWateringTime) {
            uWateringTime = uNewValue;

            pSettings.putUShort("WateringTime", uWateringTime);

            strReturn += "\r\nSe actualizó el Duración de Riego.";
          }
        }

        // =============== Fans Rest Interval =============== //
        if (request->hasArg("restinterval")) {
          uNewValue = request->arg("restinterval").toInt();

          if (uNewValue != uFansRestIntervalTime) {
            uFansRestIntervalTime = uNewValue;

            pSettings.putUInt("FanRestIntTime", uFansRestIntervalTime);

            strReturn += "\r\nSe actualizó el Intervalo de Reposo de Ventiladores.";
          }
        }

        // =============== Fans Rest Duration =============== //
        if (request->hasArg("restdur")) {
          uNewValue = request->arg("restdur").toInt();

          if (uNewValue != uFansRestDuration) {
            uFansRestDuration = uNewValue;

            pSettings.putUInt("FanRestDur", uFansRestDuration);

            strReturn += "\r\nSe actualizó la Duración de Reposo de Ventiladores.";
          }
        }

        // =============== Soil Humidity Read Interval =============== //
        if (request->hasArg("soilreadinterval")) {
          uNewValue = request->arg("soilreadinterval").toInt() * 1000;

          if (uNewValue != uSoilReadsInterval) {
            uSoilReadsInterval = uNewValue;

            pSettings.putUInt("SoilReadsInt", uSoilReadsInterval);

            strReturn += "\r\nSe actualizó el Intervalo de Lectura de Humedad del Suelo.";
          }
        }

        // =============== Environment Temperature Hysteresis =============== //
        if (request->hasArg("temphys")) {
          uNewValue = request->arg("temphys").toInt();

          if (uNewValue != uTemperatureStopHysteresis) {
            uTemperatureStopHysteresis = uNewValue;

            pSettings.putUChar("TempHysteresis", uTemperatureStopHysteresis);

            strReturn += "\r\nSe actualizó la Histéresis de Apagado por Temperatura.";
          }
        }

        // =============== Environment Humidity Hysteresis =============== //
        if (request->hasArg("humhys")) {
          uNewValue = request->arg("humhys").toInt();

          if (uNewValue != uHumidityStopHysteresis) {
            uHumidityStopHysteresis = uNewValue;

            pSettings.putUChar("HumHysteresis", uHumidityStopHysteresis);

            strReturn += "\r\nSe actualizó la Histéresis de Apagado por Humedad.";
          }
        }

        // =============== Drip per minute =============== //
        if (request->hasArg("dripperminute")) {
          uNewValue = request->arg("dripperminute").toInt();

          if (uNewValue != uDripPerMinute) {
            uDripPerMinute = uNewValue;

            pSettings.putUShort("DripPerMinute", uDripPerMinute);

            strReturn += "\r\nSe actualizó el valor de Tasa de Goteo por Minuto.";
          }
        }

        // =============== WiFi =============== //
        bool bWiFiChanges = false;

        if (request->hasArg("ssid") && request->arg("ssid") != strSSID) {
          strReturn += "\r\nSe actualizó el SSID de WiFi.";
          bWiFiChanges = true;
        }

        if (request->hasArg("ssidpwd") && request->arg("ssidpwd") != strSSIDPWD) {
          strReturn += "\r\nSe actualizó la Contraseña de WiFi.";
          bWiFiChanges = true;
        }

        if (strReturn != "") {
          if (bWiFiChanges)
            strReturn += "\r\nSe intentará conectar a la nueva Red, de no ser posible; se iniciará una Red WiFi para poder reconfigurar.";

          request->send(200, "text/plain", "MSG" + strReturn);
        }

        // =============== Current DateTime =============== //
        if (request->hasArg("settime")) {
          struct timeval tv;
          tv.tv_sec = request->arg("settime").toInt();
          tv.tv_usec = 0;

          settimeofday(&tv, nullptr);

          configTime(-3 * 3600, 0, nullptr, nullptr);

          time_t now = time(nullptr);
          struct tm timeinfo;

          localtime_r(&now, &timeinfo);
        }

        if (bWiFiChanges) {
          WiFi.disconnect(true);
          WiFi.softAPdisconnect(true);

          if (request->hasArg("ssid") && request->arg("ssid") != strSSID) {
            strSSID = request->arg("ssid");

            pSettings.putString("SSID", strSSID);
          }

          if (request->hasArg("ssidpwd") && request->arg("ssidpwd") != strSSIDPWD) {
            strSSIDPWD = request->arg("ssidpwd");

            pSettings.putString("SSIDPWD", strSSIDPWD);
          }

          if (eTaskGetState(taskhandleWiFiReconnect) == eRunning)
            vTaskSuspend(taskhandleWiFiReconnect);
        }
      } else if (request->arg("action") == "sync") {  // Request for data to refresh the panel
        // ================================================== Environment Section ================================================== //
        String strResponse = "REFRESH";
        strResponse += String(nEnvironmentTemperature) + ":" + String(nEnvironmentHumidity) + ":" + String(fEnvironmentVPD, 2);

        // ================================================== Soil Section ================================================== //
        strResponse += ":" + String(TOTAL_SOIL_HUMIDITY_SENSORS);

        for (uint8_t i = 0; i < TOTAL_SOIL_HUMIDITY_SENSORS; i++)
          strResponse += ":" + String(uSoilsHumidity[i]);

        // ================================================== Watering Section ================================================== //
        strResponse += ":" + String(uWateringElapsedTime);

        // ================================================== Current Time Section ================================================== //
        time_t now = time(nullptr);

        strResponse += ":" + String(now);

        // ================================================== Light Brightness slider position Section ================================================== //
        strResponse += ":" + String(uLightBrightness < 401 ? 0 : uLightBrightness);

        // ================================================== Fans Rest State Section ================================================== //
        strResponse += ":";

        if (bFansRest)
          strResponse += String((uFansRestDuration * 60000) - (millis() - lFansRestElapsedTime));
        else
          strResponse += "0";

        // Internal Fan
        strResponse += ":" + String(digitalRead(RELAY_1_PIN));  // HIGHT=1 A.K.A paused

        // Ventilation
        strResponse += ":" + String(digitalRead(RELAY_2_PIN));  // HIGHT=1 A.K.A paused

        // ================================================== Graph Section ================================================== //
        ConnectSD(false);

        if (bSDInit) { // V2 stolen code
          File pFile = SD.open("/metrics.log", FILE_READ);
          if (pFile) {
            size_t sizeFile = pFile.size();

            if (sizeFile > sizeLastReadFile) {
              sizeLastReadFile = sizeFile;
              const size_t sizeBlock = 512;
              const uint8_t uLinesToRead = 48;
              char cChunkBuffer[sizeBlock];
              int64_t nPos = sizeFile - sizeBlock;
              uint8_t uReadedLines = 0;

              while (nPos >= 0 && uReadedLines < uLinesToRead) {
                pFile.seek(nPos);

                size_t bytesRead = pFile.read((uint8_t*)cChunkBuffer, (nPos < sizeBlock) ? nPos + 1 : sizeBlock);
                if (bytesRead == 0)
                  break;

                for (int i = bytesRead - 1; i >= 0; i--) {
                  if (cChunkBuffer[i] == '\n' || (nPos == 0 && i == 0)) {
                    pFile.seek(nPos + i + 1);

                    char cBuffer[64];
                    uint8_t uBytesRead = pFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1);
                    cBuffer[uBytesRead] = '\0';

                    if (uBytesRead > 0) {
                      while (cBuffer[uBytesRead - 1] == '\r' || cBuffer[uBytesRead - 1] == ' ')
                        cBuffer[--uBytesRead] = '\0';

                      if (strArrayGraphData[uReadedLines] != nullptr)
                        free((void*)strArrayGraphData[uReadedLines]);

                      strArrayGraphData[uReadedLines] = strdup(cBuffer);

                      uReadedLines++;
                    }

                    if (uReadedLines >= uLinesToRead)
                      break;
                  }
                }

                if (uReadedLines >= uLinesToRead || nPos == 0)
                  break;

                nPos -= sizeBlock;
                if (nPos < 0)
                  nPos = 0;
              }
            }

            pFile.close();
          }
        }

        size_t sizeGraphArraySize = sizeof(strArrayGraphData) / sizeof(strArrayGraphData[0]);

        if (sizeGraphArraySize > 0 && strArrayGraphData[0] != nullptr) {
          strResponse += ":";

          for (int i = sizeGraphArraySize - 1; i >= 0; i--) {
            if (strArrayGraphData[i] != nullptr) {
              if (i < sizeGraphArraySize - 1)
                strResponse += ",";

              strResponse += String(strArrayGraphData[i]);
            }
          }
        }
        //////////////////////////////////////////////////////////////////////////////////////////
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain;charset=utf-8", strResponse);

        request->send(response);
        /*
          data[0] Environment Temperature
          data[1] Environment Humidity
          data[2] VPD
          data[3] Number of Soil Sensors
          data[4+i] Soil sensors values
          data[next] Current Watering elapses Time (If is 0, is not watering)
          data[next+1] Current Time (unixtimestamp)
          data[next+2] Light Brightness slider position
          data[next+3] Fans Rest Time remaining Indicator
          data[next+4] Interval Fan State
          data[next+5] Ventilation Fans State
          data[next+6] Graph Data
            Unix Timestamp|Environment Temperature|Environment Humidity|VPD|Soil Moistures,etc
        */
      }
    } else {  // Return panel content
      ConnectSD(false);

      AsyncWebServerResponse *response;

      if (bSDInit)
        response = request->beginResponse(SD, "/index.html", "text/html", false, HTMLProcessor);
      else
        response = request->beginResponse(500, "text/plain", "No hay una Tarjeta SD conectada.");

      request->send(response);
    }
  });

  AsyncWebServerHandle.begin();

  LOGGER("Web Server Started at Port: %d.", INFO, WEBSERVER_PORT);
}

void loop() {
/*#ifdef ENABLE_SERIAL_LOGGER
  LOGGER("Free HEAP: %d", INFO, ESP.getFreeHeap());
#endif*/
  unsigned long lCurrentMillis = millis();

  if (lCurrentMillis - lEachSecondMillis >= 1000) {
    lEachSecondMillis = lCurrentMillis;
    time_t now = time(nullptr);
    struct tm *timeInfo = localtime(&now);

    if (WiFi.status() != WL_CONNECTED && eTaskGetState(taskhandleWiFiReconnect) != eRunning)
      vTaskResume(taskhandleWiFiReconnect);

    if (lCurrentMillis - lEachMinuteMillis >= 60000) {
      lEachMinuteMillis = lCurrentMillis;

      if (!bConnectedToInternet)  // If have no Internet Connection
        if (CheckInternetConnection()) {  // Do Ping to NTP Server
          LOGGER("Resynchronizing Datetime...", INFO);
          
          GetDateTime(false);  // Re-sync Datetime
        }
    }

    if (bSDInit)
      WriteToSD("/time", String((uint32_t)now), false);

    // ================================================== Environment Section ================================================== //
    GetEnvironmentParameters(nEnvironmentTemperature, nEnvironmentHumidity, fEnvironmentVPD);  // Get environment params to store in global vals

    // ================================================== Soil Section ================================================== //
    if (lCurrentMillis - lSoilHumidityElapsedReadTime >= uSoilReadsInterval) {
      lSoilHumidityElapsedReadTime = lCurrentMillis;

      LOGGER("Environment Temperature: %d°C Humidity: %d%% VPD: %.2fkPa.", INFO, nEnvironmentTemperature, nEnvironmentHumidity, fEnvironmentVPD);

      for (uint8_t i = 0; i < TOTAL_SOIL_HUMIDITY_SENSORS; i++) {
        uSoilsHumidity[i] = GetSoilHumidity(i);

        LOGGER("Soil Sensor %d Humidity: %d%%.", INFO, i, uSoilsHumidity[i]);
      }
    }

    // ================================================== Light Section ================================================== //
    // If current time in between Start Stop range
    if ((uEffectiveStartLights < uEffectiveStopLights && timeInfo->tm_hour >= uEffectiveStartLights && timeInfo->tm_hour < uEffectiveStopLights) || (uEffectiveStartLights >= uEffectiveStopLights && (timeInfo->tm_hour >= uEffectiveStartLights || timeInfo->tm_hour < uEffectiveStopLights))) {
      if (digitalRead(RELAY_0_PIN)) {
        digitalWrite(RELAY_0_PIN, LOW);

        LOGGER("Light Started.", INFO);
      }
    } else {  // If current time is out of ON range
      if (!digitalRead(RELAY_0_PIN)) {
        digitalWrite(RELAY_0_PIN, HIGH);

        LOGGER("Stopped Light.", INFO);
      }
    }

    // ================================================== Ventilation Section ================================================== //
    if (!bFansRest) {
      if (nEnvironmentTemperature > 0 && nEnvironmentHumidity > 0) {  // Si los valores de Temperatura y Humedad Ambiente no son 0
        // ================================================== Internal Fan Section ================================================== //
        if (nEnvironmentTemperature >= uStartFanTemperature) {
          if (digitalRead(RELAY_1_PIN)) {
            digitalWrite(RELAY_1_PIN, LOW);

            LOGGER("Internal Fan Started.", INFO);
          }
        } else if (nEnvironmentTemperature <= uStartFanTemperature - uTemperatureStopHysteresis) {
          if (!digitalRead(RELAY_1_PIN)) {
            digitalWrite(RELAY_1_PIN, HIGH);

            LOGGER("Stopped Internal Fan.", INFO);
          }
        }

        // ================================================== Ventilation Section ================================================== //
        // =============== Control by Temperature =============== //
        if (nEnvironmentTemperature >= uStartVentilationTemperature && uStartVentilationTemperature > 0)
          bVentilationByTemperature = true;
        else if (nEnvironmentTemperature <= uStartVentilationTemperature - uTemperatureStopHysteresis)
          bVentilationByTemperature = false;

        // =============== Control by Humidity =============== //
        if (nEnvironmentHumidity >= uStartVentilationHumidity && uStartVentilationHumidity > 0)
          bVentilationByHumidity = true;
        else if (nEnvironmentHumidity <= uStartVentilationHumidity - uHumidityStopHysteresis)
          bVentilationByHumidity = false;

        if (bVentilationByTemperature || bVentilationByHumidity) {  // Si la Ventilación debería estar encendida por Temperatura o Humedad Ambiente
          if (digitalRead(RELAY_2_PIN)) {
            digitalWrite(RELAY_2_PIN, LOW);

            LOGGER("Ventilation Started.", INFO);
          }
        } else {
          if (!digitalRead(RELAY_2_PIN)) {
            digitalWrite(RELAY_2_PIN, HIGH);

            LOGGER("Stopped Ventilation.", INFO);
          }
        }
      }
    }

    // ================================================== Watering Section ================================================== //
    if (digitalRead(RELAY_3_PIN) && (uStartWateringHumidity > 0 || uWateringInterval > 0)) {  // Si el relé de la Bomba de Riego está OFF (HIGH) y el nivel de Humedad requerido para Regar es mayor a 0 o si el Intervalo para Regar es mayor a 0
      if (millis() - lStartupTime >= uSoilReadsInterval * 1.5) {  // Esperar al menos el doble de tiempo que se espera para leer los valores de Humedad del suelo, así es 100% seguro que no se inicie el Riego anticipadamente
        bool bStartWatering = false;

        if (uWateringMode == 0) {
          uint16_t uCombinedHumidity = 0;

          for (uint8_t i = 0; i < TOTAL_SOIL_HUMIDITY_SENSORS; i++)
            uCombinedHumidity += uSoilsHumidity[i];

          if (uCombinedHumidity / TOTAL_SOIL_HUMIDITY_SENSORS <= uStartWateringHumidity) {  // Si el promedio de Humedad es igual o menor que el valor de Humedad necesario para iniciar el Riego
            bStartWatering = true;

            LOGGER("Watering Started by Soil Humidity Method.", INFO);
          }
        } else {
          if (lCurrentMillis - lWateringIntervalElapsedTime >= uWateringInterval) {
            lWateringIntervalElapsedTime = lCurrentMillis;
            bStartWatering = true;

            LOGGER("Watering Started by Interval Method.", INFO);
          }
        }

        if (bStartWatering) {
          uWateringElapsedTime = 0; // Por las dudas reiniciar el contador de Tiempo de Riego transcurrido

          digitalWrite(RELAY_3_PIN, LOW);  // Activar el relé de la Bomba de Riego
        }
      }
    }

    if (!digitalRead(RELAY_3_PIN)) {  // Si el relé de la Bomba de Riego está ON (LOW)
      if (uWateringElapsedTime >= uWateringTime) {  // Si el Tiempo de Riego transcurrido es mayor o igual al Tiempo de Riego definido
        // Actualizar los valores de Humedad del Suelo. Para evitar que se ejecute instantaneamente otro Riego
        for (uint8_t i = 0; i < TOTAL_SOIL_HUMIDITY_SENSORS; i++)
          uSoilsHumidity[i] = GetSoilHumidity(i);

        uWateringElapsedTime = 0;  // Reiniciar el contador de Tiempo de Riego transcurrido

        digitalWrite(RELAY_3_PIN, HIGH);  // Apagar el Relé de la Bomba de Riego

        LOGGER("Stopped Watering.", INFO);
      } else {  // Si el Tiempo de Riego es menor al Tiempo de Riego definido
        uWateringElapsedTime++;  // Incrementar el contador de Tiempo de Riego transcurrido +1
      }
    }

    // ================================================== Store Data for Graph Section ================================================== //
    if (millis() - lStartupTime >= uSoilReadsInterval + 60000 /*+1 Minute*/ && lCurrentMillis - lLastStoreElapsedTime >= 3600000) {
      lLastStoreElapsedTime = lCurrentMillis;

      String strValues = String(now) + "|" + String(nEnvironmentTemperature) + "|" + String(nEnvironmentHumidity) + "|" + String(fEnvironmentVPD, 2);

      for (uint8_t i = 0; i < TOTAL_SOIL_HUMIDITY_SENSORS; i++)
       strValues += "|" + String(uSoilsHumidity[i]);

      WriteToSD("/metrics.log", strValues, true);
    }

    // ================================================== Fans Rest Time Section ================================================== //
    if (lCurrentMillis - lFansRestIntervalTime >= uFansRestIntervalTime * 60000) {  // Si transcurrió el Tiempo de Intervalo entre Descanso y Descanso
      lFansRestIntervalTime = lCurrentMillis;
      lFansRestElapsedTime = lCurrentMillis;
      bFansRest = true;

      digitalWrite(RELAY_1_PIN, HIGH);  // Internal Fan
      digitalWrite(RELAY_2_PIN, HIGH);  // Ventilation

      LOGGER("Fans Rest mode started.", INFO);
    }

    if (bFansRest) {
      if (lCurrentMillis - lFansRestElapsedTime >= uFansRestDuration * 60000) {  // Si transcurrió el Tiempo de Descanso
        bFansRest = false;
        lFansRestElapsedTime = 0;

        LOGGER("Fans Rest mode completed.", INFO);
      }
    }
  }
}
