///////////////////////////////////////////////////////////////////////////////////
//          _________________________________________________________________    //
//         /                                                                /\   //
//        /  _   __    _                   __                   ______     / /\  //
//       /  / | / /   (_)  _____  ____    / /  ____ _   _____  / ____/  __/ /    //
//      /  /  |/ /   / /  / ___/ / __ \  / /  / __ `/  / ___/ / / __   /\_\/     //
//     /  / /|  /   / /  / /__  / /_/ / / /  / /_/ /  (__  ) / /_/ /  /_/        //
//    /  /_/ |_/   /_/   \___/  \____/ /_/   \__,_/  /____/  \____/    /\        //
//   /                                                                / /        //
//  /________________________________________________________________/ /         //
//  \________________________________________________________________\/          //
//   \    \    \    \    \    \    \    \    \    \    \    \    \    \          //
//                               Version 2 (2025)                                //
///////////////////////////////////////////////////////////////////////////////////
#include <SD.h> // https://docs.arduino.cc/libraries/sd/#SD%20class
#include <WiFi.h>
#include <time.h>
#include <ESPmDNS.h>
#include <SimpleDHT.h>
#include <ESPAsyncWebServer.h>
/* TODO:
  1) Perfiles para growing, flowering y drying                                                            DONE!
  2) Curva de riego, que riegue primero x cantidad, pasado x tiempo riegue x*Multiplicador o lo que sea
*/
struct RelayPin {
  const char* Name; // Channel name
  uint8_t Pin;      // Pin number
};

struct Settings {
  uint8_t StartLightTime;
  uint8_t StopLightTime;
  uint16_t LightBrightness;

  uint8_t StartInternalFanTemperature;

  uint8_t StartVentilationTemperature;
  uint8_t StartVentilationHumidity;

  uint8_t WateringCriteria;
  uint64_t StartWateringInterval;
  uint8_t StartWateringHumidity;
  uint16_t WateringDuration;
};

// NOTES:
// Default IP for AP mode is: 192.168.4.1
// If Environment Humidity or Temperature Reads 0, the fans never gonna start.

// Definitions
#define ENABLE_SERIAL_LOGGER  // Use this when debugging
//#define ENABLE_SD_LOGGING   // Use this to save logs to SD Card
#define ENABLE_AP_ALWAYS      // Use this to enable always the Access Point. Else it just enable when have no internet connection

#define MAX_PROFILES 3  // 0 Vegetative (Filename: veg), 1 Flowering (Filename: flo), 2 Drying (Filename: dry)

#define MAX_GRAPH_MARKS 48  // How much logs show in Web Panel Graph
#define GRAPH_MARKS_INTERVAL 3600000  // Intervals in which values ​​are stored for the graph // TODO: Esto lo podría incluir en la configuración interna. Para poder cambiarlo desde el Panel Web

#define WIFI_MAX_RETRYS 5 // Max Wifi reconnection attempts

#define WEBSERVER_PORT 80

#define DNS_ADDRESS "indoor"  // http://"DNS_ADDRESS".local/
#define ACCESSPOINT_NAME "ESP32_Indoor"

#define TIMEZONE_UTC_OFFSET (-3 * 3600) // Timezone // TODO: Esto lo podría incluir en la configuración interna. Para poder cambiarlo desde el Panel Web
#define TIMEZONE_DST_OFFSET 0 // Daylight Savings Time

#define S8050_FREQUENCY 300   // https://www.mouser.com/datasheet/2/149/SS8050-117753.pdf
#define S8050_RESOLUTION 12
#define S8050_MAX_VALUE 4095  // Cuz is 12 bits of resolution

#define HW080_MIN 0         // https://cms.katranji.com/web/content/723081
#define HW080_MAX 2800      // I'm using 20k resistors, so the max value never go up to 4095
#define HW080_MAX_READS 10  // To get good soil humidity average

#define DHT_MAX_READS 5 // To get average

// Pins
#define DHT_DATA_PIN 16 // https://www.mouser.com/datasheet/2/758/DHT11-Technical-Data-Sheet-Translated-Version-1143054.pdf

#define SD_CS_PIN 5 // Chip select for Enable/Disable SD Card

#define S8050_PWM_PIN 33  // I'm using a 1K resistor in serie in BASE Pin (Light Brightness controller)

#define HW080_VCC_PIN 32  // I Enable this pin when want to read and disable it to prevent electrolysis

const RelayPin pRelayModulePins[] = {
  { "Lights", 14 },           // Pin for Channel 0 of Relay Module
  { "Internal Fan", 27 },     // Pin for Channel 1 of Relay Module
  { "Ventilation Fan", 26 },  // Pin for Channel 2 of Relay Module
  { "Water Pump", 25 }        // Pin for Channel 3 of Relay Module
};

const uint8_t nSoilHumidityPins[] = {
  34,  // Soil Humidity Sensor 0
  35   // Soil Humidity Sensor 1
};

// Global Constants
const uint8_t nRelayModulePinsCount = sizeof(pRelayModulePins) / sizeof(pRelayModulePins[0]);
const uint8_t nSoilHumidityPinsCount = sizeof(nSoilHumidityPins) / sizeof(nSoilHumidityPins[0]);

// Global Variables
const char* g_strWebServerFiles[] = {
  "fan.webp",
  "chart.js"  // Add here files to be server by the webserver
};

const String g_strSoilGraphColor[] = {
  "#B57165",
  "#784B43"  // Add here more colors for graph
};

enum ERR_TYPE { INFO, WARN, ERROR };

// Settings storage Variables
char g_cSSID[32];
char g_cSSIDPWD[32];

uint32_t g_nFansRestInterval = 0;
uint32_t g_nFansRestDuration = 0;

uint32_t g_nSoilReadsInterval = 0;

uint8_t g_nTemperatureStopHysteresis = 0;
uint8_t g_nHumidityStopHysteresis = 0;
uint16_t g_nDripPerMinute = 0;

Settings g_pSettings[MAX_PROFILES] = {};
///////////////////////////////////////////
unsigned long g_ulStartUpTime = 0;
bool g_bIsSDInit = false;
uint8_t g_nCurrentProfile = 0;
uint8_t g_nEffectiveStartLights = 0;
uint8_t g_nEffectiveStopLights = 0;
uint8_t g_nEnvironmentTemperature = 0;
uint8_t g_nEnvironmentHumidity = 0;
float g_fEnvironmentVPD = 0.0f;
uint8_t g_nSoilsHumidity[nSoilHumidityPinsCount] = {};
uint16_t g_nWateringElapsedTime = 0;
unsigned long g_ulFansRestElapsedTime = 0;
bool g_bFansRest = false;
const char* g_strArrayGraphData[MAX_GRAPH_MARKS] = {};
unsigned long g_ulSoilHumidityElapsedReadTime = 0;
unsigned long g_ulWateringIntervalElapsedTime = 0;
unsigned long g_ulLastStoreElapsedTime = 0;

// Global Handles, Interface & Instances
AsyncWebServer pWebServer(WEBSERVER_PORT);  // Asynchronous web server instance listening on WEBSERVER_PORT
SimpleDHT11 pDHT11(DHT_DATA_PIN);           // Interface to DHT11 Temperature & Humidity sensor
TaskHandle_t pWiFiReconnect;                // Task handle for Wifi reconnect logic running on core 0

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads temperature and humidity from a DHT11 sensor, retrying a defined number of times if necessary. 
// Calculates VPD (Vapor Pressure Deficit) based on the readings, or logs an error if the readings are invalid.
void GetEnvironmentParameters(uint8_t &nTemperature, uint8_t &nHumidity, float &fVPD) {
  int nError = SimpleDHTErrSuccess;
  uint8_t nReadTrysCount = 0;
  byte bTemperature = 0, bHumidity = 0;

  do {  // Try read Temp & Humidity values from Environment
    if ((nError = pDHT11.read(&bTemperature, &bHumidity, NULL)) != SimpleDHTErrSuccess) { // Get Environment Temperature and Humidity
      nReadTrysCount++;

      LOGGER("Read DHT11 failed, Error=%d, Duration=%d.", ERROR, SimpleDHTErrCode(nError), SimpleDHTErrDuration(nError));

      delay(100);
    }
  } while (nError != SimpleDHTErrSuccess && nReadTrysCount < DHT_MAX_READS);  // Repeat while it fails and not reach max trys

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

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads the average humidity value from a soil sensor. 
// Excites the sensor by setting the VCC pin high and waits for stable readings. 
// Takes multiple analog readings, sums them, and normalizes the result to a percentage scale (0-100). 
// After readings, the VCC pin is set low to stop electrolysis. 
// Returns the normalized humidity value as an integer between 0 and 100.
uint8_t GetSoilHumidity(uint8_t nSensorNumber) {
  digitalWrite(HW080_VCC_PIN, HIGH);  // Put Pin output in High to excite the moisture sensors

  delay(10);  // Small Wait to obtain an stable reading

  uint32_t nCombinedValues = 0;

  for (uint8_t i = 0; i < HW080_MAX_READS; i++) {
    nCombinedValues += analogRead(nSoilHumidityPins[nSensorNumber]);
    delay(100); // Small delay between reads
  }

  digitalWrite(HW080_VCC_PIN, LOW); // Put pin output in low to stop electrolysis

  return constrain(map(nCombinedValues / HW080_MAX_READS, HW080_MIN, HW080_MAX, 0, 100), 0, 100);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Searches for a relay channel by its name.
// Returns the associated pin number if found.
// Returns -1 if the name does not match any configured relay.
int GetPinByName(const char* Name) {
    for (int i = 0; i < nRelayModulePinsCount; ++i) {
      if (strcmp(pRelayModulePins[i].Name, Name) == 0)
        return pRelayModulePins[i].Pin;
    }

    return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void TrimTrailingWhitespace(char* str) {
  int nLen = strlen(str);
  while (nLen > 0 && isspace(str[nLen - 1])) {
    str[--nLen] = '\0';
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tries to initialize the SD card file system if not already initialized.
// If bShowSuccessLog is true, logs a success message when initialization succeeds.
// Sets g_bIsSDInit to true if the SD card is ready.
void ConnectSD(bool bShowSuccessLog) {
  if (!SD.open("/")) {
    SD.end();

    g_bIsSDInit = SD.begin(SD_CS_PIN);
    if (!g_bIsSDInit)
      LOGGER("Failed to initialize SD Card File System.", ERROR);

    if (bShowSuccessLog)
      LOGGER("SD Card File System initialized.", INFO);
  } else {  // Just in case
    g_bIsSDInit = true;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Writes a line of text to the specified file on the SD card.
// If strFileName is "logging_", appends the current date to create a log file name (e.g., logging_2025_04_30.log).
// If bAppend is true, appends to the file; otherwise, overwrites it.
// Automatically initializes the SD card if needed (without logging success).
void WriteToSD(String strFileName, String strText, bool bAppend) {
  ConnectSD(false);

  if (g_bIsSDInit) {
    if (strFileName == "logging_") {
      time_t timeNow = time(nullptr);
      struct tm *timeInfo = localtime(&timeNow);

      strFileName += String(timeInfo->tm_year + 1900) + "_" + String(timeInfo->tm_mon + 1) + "_" + String(timeInfo->tm_mday) + ".log";
    }

    File pFile = SD.open(strFileName.c_str(), (bAppend ? FILE_APPEND : FILE_WRITE));
    if (pFile)
      pFile.println(strText.c_str());

    pFile.close();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Logs a formatted message with a severity prefix (INFO, WARNING, ERROR, etc.).
// Outputs to SD card if ENABLE_SD_LOGGING is defined, and/or to Serial if ENABLE_SERIAL_LOGGER is defined.
// Formats the message using printf-style syntax with variable arguments.
// Automatically prepends a severity label to the message (e.g., [INFO], [ERROR]).
void LOGGER(const char *format, ERR_TYPE nType, ...) {
#if defined(ENABLE_SD_LOGGING) || defined(ENABLE_SERIAL_LOGGER)
  va_list args;

  char cPrefix[11], cBuffer[1024];

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

#ifdef ENABLE_SD_LOGGING
  WriteToSD("/logging_", String(cBuffer), true);
#endif
#ifdef ENABLE_SERIAL_LOGGER
  Serial.println(cBuffer);
#endif
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Attempts to reconnect to the configured WiFi network using the global SSID and password.
// If ENABLE_AP_ALWAYS is not defined, it starts an Access Point (ACCESSPOINT_NAME) temporarily to allow reconfiguration while trying to reconnect.
// Tries to connect up to WIFI_MAX_RETRYS times, waiting 1 second between each attempt.
// Logs success with IP address if connected, or an error if all attempts fail.
// If AP was started, it is stopped after a successful connection.
// Finally, suspends the task until it is explicitly resumed again.
void Thread_WifiReconnect(void *parameter) {
#if !defined(ENABLE_AP_ALWAYS)
  LOGGER("Starting Access Point (SSID: " + String(ACCESSPOINT_NAME) + ") mode for reconfiguration...", INFO);

  WiFi.mode(WIFI_AP_STA); // Set dual mode, Access Point & Station

  vTaskDelay(100 / portTICK_PERIOD_MS); // Delay to stabilize AP

  WiFi.softAP(ACCESSPOINT_NAME); // Start Access Point, while try to connect to Wifi
#endif

  LOGGER("Trying to reconnect Wifi...", INFO);

  WiFi.begin(g_cSSID, g_cSSIDPWD);

  uint8_t nConnectTrysCount = 0;

  while (WiFi.status() != WL_CONNECTED && nConnectTrysCount < WIFI_MAX_RETRYS) {
    nConnectTrysCount++;
    vTaskDelay(1000 / portTICK_PERIOD_MS);  // Wait 1 second before trying again
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOGGER("Connected to Wifi SSID: %s PASSWORD: %s. IP: %s.", INFO, g_cSSID, g_cSSIDPWD, WiFi.localIP().toString().c_str());

#if !defined(ENABLE_AP_ALWAYS)
    WiFi.softAPdisconnect(true);

    LOGGER("Access Point disconnected.", INFO);
#endif
  } else {
    LOGGER("Max Wifi reconnect attempts reached.", ERROR);
  }

  vTaskSuspend(pWiFiReconnect); // Suspends the task until needed again
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Generates and returns dynamic HTML content or variable values based on the input key `var`. Just for the first web request.
// Supports returning environmental data, system states, and dynamically generated HTML blocks
// for insertion into templates. Used with templating engines (e.g., AsyncWebServer's .setTemplateProcessor).
String HTMLProcessor(const String &var) {
  if (var == "DPM") {
    return String(g_nDripPerMinute);
  } else if (var == "ENVTEMP") {
    return String(g_nEnvironmentTemperature);
  } else if (var == "ENVHUM") {
    return String(g_nEnvironmentHumidity);
  } else if (var == "VPDSECTION") {
    String strHTMLColor = "#FE7F96", strState = "Zona de Peligro";

    if (g_fEnvironmentVPD >= 0.4 && g_fEnvironmentVPD <= 0.8) {  // Propagation / Early Veg
      strHTMLColor = "#6497C9";
      strState = "Propagacíon/Inicio del Vegetativo";
    } else if (g_fEnvironmentVPD > 0.8 && g_fEnvironmentVPD <= 1.2) {  // Late Veg / Early Flower
      strHTMLColor = "#7FC794";
      strState = "Vegetativo/Inicio de Floración";
    } else if (g_fEnvironmentVPD > 1.2 && g_fEnvironmentVPD <= 1.6) {  // Mid / Late Flower
      strHTMLColor = "#F9AE54";
      strState = "Floración";
    }

    return "<tr><td>Déficit de Presión de Vapor:</td><td><font id=vpd color=" + strHTMLColor + ">" + String(g_fEnvironmentVPD, 2) + "</font>kPa</td><td>(<font id=vpdstate color=" + strHTMLColor + ">" + strState + "</font>)</td></tr>";
  } else if (var == "SOILSECTION") {
    String strReturn;

    for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++) {
      strReturn += "<tr><td>Humedad de Maceta " + String(i) + ":</td><td><p id=soil" + String(i) + ">" + String(g_nSoilsHumidity[i]) + "&#37;</p></td>";

      if (i == 0)
        strReturn += "<td rowspan=" + String(nSoilHumidityPinsCount) + " id=wateringstate style=color:#72D6EB>" + (g_nWateringElapsedTime > 0 ? "Regando...<br>Tiempo Transcurrido: " + String(g_nWateringElapsedTime) + " segundos" : "") + "</td>";

      strReturn += "</tr>";
    }

    return strReturn;
  } else if (var == "CURRENTTIME") {
    time_t timeNow = time(nullptr);
    struct tm *timeInfo = localtime(&timeNow);
    auto timeformat = [](int value) {
      return String(value).length() == 1 ? "0" + String(value) : String(value);
    };

    return String(timeformat(timeInfo->tm_hour) + ":" + timeformat(timeInfo->tm_min) + ":" + timeformat(timeInfo->tm_sec));
  } else if (var == "PROFILE") {
    return String(g_nCurrentProfile);
  } else if (var == "STARTLIGHT") {
    return String(g_pSettings[g_nCurrentProfile].StartLightTime);
  } else if (var == "STOPLIGHT") {
    return String(g_pSettings[g_nCurrentProfile].StopLightTime);
  } else if (var == "MAXBRIGHT") {
    return String(S8050_MAX_VALUE);
  } else if (var == "BRIGHTLEVEL") {
    return String(g_pSettings[g_nCurrentProfile].LightBrightness < 401 ? 0 : g_pSettings[g_nCurrentProfile].LightBrightness); // WARNING: Hardcode offset
  } else if (var == "STAINTFAN") {
    return String(g_pSettings[g_nCurrentProfile].StartInternalFanTemperature);
  } else if (var == "TEMPSTARTVENT") {
    return String(g_pSettings[g_nCurrentProfile].StartVentilationTemperature);
  } else if (var == "HUMSTARTVENT") {
    return String(g_pSettings[g_nCurrentProfile].StartVentilationHumidity);
  } else if (var == "RESTSTATE") {
    String strReturn;

    if (g_ulFansRestElapsedTime > 0) {
      strReturn = "En Reposo...<br>Tiempo Restante: ";

      unsigned long ulTimeRemaining = (g_nFansRestDuration * 60000) - (millis() - g_ulFansRestElapsedTime);

      if (ulTimeRemaining < 60000) {
        strReturn += String(ulTimeRemaining / 1000) + " segundos";
      } else {
        unsigned long ulMinutes = ulTimeRemaining / 60000;
        unsigned long ulSeconds = (ulTimeRemaining % 60000) / 1000;

        strReturn += String(ulMinutes) + (ulMinutes == 1 ? " minuto" : " minutos");

        if (ulSeconds > 0)
          strReturn += " y " + String(ulSeconds) + " segundos";
      }
    }

    return strReturn;
  } else if (var == "WATERINGMODE") {
    String strReturn;

    if (g_pSettings[g_nCurrentProfile].WateringCriteria == 1)
      strReturn = "checked";

    return strReturn;
  } else if (var == "STAWATHUM") {
    if (g_pSettings[g_nCurrentProfile].WateringCriteria == 0)
      return String(g_pSettings[g_nCurrentProfile].StartWateringHumidity);
    else
      return String(g_pSettings[g_nCurrentProfile].StartWateringInterval / 3600000);
  } else if (var == "WATERINGTIME") {
    return String(g_pSettings[g_nCurrentProfile].WateringDuration);
  } else if (var == "RESTINTERVAL") {
    return String(g_nFansRestInterval);
  } else if (var == "RESTDUR") {
    return String(g_nFansRestDuration);
  } else if (var == "SOILDREADINT") {
    return String(g_nSoilReadsInterval / 1000);
  } else if (var == "TEMPSTOPHYS") {
    return String(g_nTemperatureStopHysteresis);
  } else if (var == "HUMSTOPHYS") {
    return String(g_nHumidityStopHysteresis);
  } else if (var == "SSID") {
    return String(g_cSSID);
  } else if (var == "SSIDPWD") {
    return String(g_cSSIDPWD);
  } else if (var == "SOILLINES") {
    String strReturn;

    for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++)
      strReturn += ",{label:'Humedad de Maceta " + String(i) + "',borderColor:'" + g_strSoilGraphColor[i] + "',backgroundColor:'" + g_strSoilGraphColor[i] + "',symbol:'%%',yAxisID:'" + (3 + i) + "'}";

    return strReturn;
  }

  return String();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Loads profile configuration data from SD card files (/veg, /flo, /dry) into the g_pSettings array.
// If the SD card is not initialized, attempts to initialize it by calling ConnectSD(false).
// For the current active profile (g_nCurrentProfile), updates effective light start/stop times
// by normalizing hour values (24 to 0 for start, 0 to 24 for stop).
void ProfilesLoader() {
  ConnectSD(false);

  if (g_bIsSDInit) {
    for (uint8_t i = 0; i < MAX_PROFILES; i++) {
      File pFile = SD.open(((i == 0) ? "/veg" : ((i == 1) ? "/flo" : "/dry")), FILE_READ);
      if (pFile) {
        char cBuffer[64];
        uint8_t nLine = 0;

        while (pFile.available()) {
          cBuffer[pFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';
          TrimTrailingWhitespace(cBuffer);

          switch (nLine) {
            case 0: // START LIGHT TIME
              {
                g_pSettings[i].StartLightTime = atoi(cBuffer);

                if (g_nCurrentProfile == i) // Only set effective start lights time if is current profile loop
                  g_nEffectiveStartLights = (g_pSettings[g_nCurrentProfile].StartLightTime == 24) ? 0 : g_pSettings[g_nCurrentProfile].StartLightTime;  // Stores the effective light start hour, converting 24 to 0 (midnight)
              }
              break;
            case 1: // STOP LIGHT TIME
              {
                g_pSettings[i].StopLightTime = atoi(cBuffer);

                if (g_nCurrentProfile == i) // Only set effective stop lights time if is current profile loop
                  g_nEffectiveStopLights = (g_pSettings[g_nCurrentProfile].StopLightTime == 0) ? 24 : g_pSettings[g_nCurrentProfile].StopLightTime; // Stores the effective light stop hour, converting 0 to 24 (midnight)
              }
              break;
            case 2: // LIGHT BRIGHTNESS LEVEL
              g_pSettings[i].LightBrightness = atoi(cBuffer);
              break;
            ///////////////////////////////////////////////////
            case 3: // INTERNAL FAN TEMPERATURE START
              g_pSettings[i].StartInternalFanTemperature = atoi(cBuffer);
              break;
            ///////////////////////////////////////////////////
            case 4: // VENTILATION TEMPERATURE START
              g_pSettings[i].StartVentilationTemperature = atoi(cBuffer);
              break;
            case 5: // VENTILATION HUMIDITY START
              g_pSettings[i].StartVentilationHumidity = atoi(cBuffer);
              break;
            ///////////////////////////////////////////////////
            case 6: // WATERING CRITERIA (BY HUMIDITY OR BY ELAPSED TIME)
              g_pSettings[i].WateringCriteria = atoi(cBuffer);
              break;
            case 7: // WATERING INTERVAL
              g_pSettings[i].StartWateringInterval = atoi(cBuffer);
              break;
            case 8: // WATERING HUMIDITY START
              g_pSettings[i].StartWateringHumidity = atoi(cBuffer);
              break;
            case 9: // WATERING DURATION
              g_pSettings[i].WateringDuration = atoi(cBuffer);
              break;
          }

          nLine++;
        }

        pFile.close();
      }
    }
  }
}

void setup() {
#ifdef ENABLE_SERIAL_LOGGER
  Serial.begin(9600);
  delay(3000);  // Small delay cuz that trash don't print the initial log
#endif

  LOGGER("========== Indoor Controller Started ==========", INFO);
  
  g_ulStartUpTime = millis();

  LOGGER("Initializing Pins...", INFO);

  char cLogBuffer[32];  // WARNING: Take care, not big size

  for (uint8_t i = 0; i < nRelayModulePinsCount; ++i) {
    pinMode(pRelayModulePins[i].Pin, OUTPUT);     // Set Pin Mode
    digitalWrite(pRelayModulePins[i].Pin, HIGH);  // Set default Pin State

    snprintf(cLogBuffer, sizeof(cLogBuffer), "%s Pin Done!", pRelayModulePins[i].Name);
    LOGGER(cLogBuffer, INFO);
  }

  ledcAttach(S8050_PWM_PIN, S8050_FREQUENCY, S8050_RESOLUTION);
  ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - (g_pSettings[g_nCurrentProfile].LightBrightness < 401 ? 0 : g_pSettings[g_nCurrentProfile].LightBrightness));  // WARNING: Hardcode offset
  LOGGER("Light Brightness Pin Done!", INFO);

  pinMode(HW080_VCC_PIN, OUTPUT);
  digitalWrite(HW080_VCC_PIN, LOW);
  LOGGER("Power Pin for Soil Humidity Sensors Done!", INFO);

  for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++) {
    pinMode(nSoilHumidityPins[i], INPUT);

    snprintf(cLogBuffer, sizeof(cLogBuffer), "Soil Humidity Pin %d Done!", i);
    LOGGER(cLogBuffer, INFO);
  }

  ConnectSD(true);  // Try to init SD Card

  LOGGER("Loading Settings & Time...", INFO);

  if (g_bIsSDInit) {
    File pSettingsFile = SD.open("/settings", FILE_READ);
    if (pSettingsFile) {
      char cBuffer[64];
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // SSID
      TrimTrailingWhitespace(cBuffer);
      strncpy(g_cSSID, cBuffer, sizeof(g_cSSID));
      g_cSSID[sizeof(g_cSSID) - 1] = '\0';
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // SSID PASSWORD
      TrimTrailingWhitespace(cBuffer);
      strncpy(g_cSSIDPWD, cBuffer, sizeof(g_cSSIDPWD));
      g_cSSID[sizeof(g_cSSIDPWD) - 1] = '\0';
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // SELECTED PROFILE
      g_nCurrentProfile = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // FANS REST INTERVAL
      g_nFansRestInterval = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // FANS REST DURATION
      g_nFansRestDuration = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // SOIL MOISTURE READS INTERVAL
      g_nSoilReadsInterval = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // TEMPERATURE HYSTERESIS TO STOP FANS
      g_nTemperatureStopHysteresis = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // MOISTURE HYSTERESIS TO STOP FANS
      g_nHumidityStopHysteresis = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // DRIPS PER MINUTE
      g_nDripPerMinute = atoi(cBuffer);
      ///////////////////////////////////////////////////
      ProfilesLoader();  // LOAD PROFILES VALUES
    } else {
      LOGGER("Failed to open Settings file.", ERROR);
    }

    pSettingsFile.close();

    if (SD.exists("/time")) {
      File pTimeFile = SD.open("/time", FILE_READ);
      if (pTimeFile) {
        LOGGER("Getting Datetime from SD Card...", INFO);

        struct timeval tv;
        tv.tv_sec = pTimeFile.parseInt();
        tv.tv_usec = 0;

        settimeofday(&tv, nullptr);

        configTime(TIMEZONE_UTC_OFFSET, TIMEZONE_DST_OFFSET, nullptr, nullptr);

        time_t timeNow = time(nullptr);
        struct tm timeInfo;

        localtime_r(&timeNow, &timeInfo);

        LOGGER("Current Datetime: %04d-%02d-%02d %02d:%02d:%02d.", INFO, timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
      } else {
        LOGGER("Failed to open Time file.", ERROR);
      }

      pTimeFile.close();
    }
  } else {
    LOGGER("SD initialization failed. Settings & Time will not be loaded, but the system will not restart to avoid unexpected relay behavior.", ERROR);
  }

  LOGGER("Initializing Wifi...", INFO);

#ifdef ENABLE_AP_ALWAYS
  WiFi.mode(WIFI_AP_STA); // Set dual mode, Access Point & Station

  WiFi.softAP(ACCESSPOINT_NAME);

  LOGGER("Access Point active. AP IP: %s", INFO, WiFi.softAPIP().toString().c_str());
#else
  WiFi.mode(WIFI_STA);    // Only Station mode
#endif

  if (g_bIsSDInit) {
    WiFi.begin(g_cSSID, g_cSSIDPWD);

    uint8_t nConnectTrysCount = 0;

    while (WiFi.status() != WL_CONNECTED && nConnectTrysCount < WIFI_MAX_RETRYS) {
      nConnectTrysCount++;
      delay(1000);  // Wait 1 second before trying again
    }

    if (WiFi.status() == WL_CONNECTED)
      LOGGER("Connected to Wifi SSID: %s PASSWORD: %s. IP: %s.", INFO, g_cSSID, g_cSSIDPWD, WiFi.localIP().toString().c_str());
    else
      LOGGER("Max Wifi reconnect attempts reached.", ERROR);
  }

  LOGGER("Initializing mDNS...", INFO);

  if (MDNS.begin(DNS_ADDRESS)) {
    MDNS.addService("http", "tcp", WEBSERVER_PORT);

    LOGGER("mDNS Started at: %s.local Service: HTTP, Protocol: TCP, Port: %d.", INFO, DNS_ADDRESS, WEBSERVER_PORT);
  } else {
    LOGGER("Failed to initialize mDNS.", ERROR);
  }

  LOGGER("Creating Wifi reconnect task thread...", INFO);

  xTaskCreatePinnedToCore(Thread_WifiReconnect, "Wifi Reconnect Task", 4096, NULL, 1, &pWiFiReconnect, 0);
  vTaskSuspend(pWiFiReconnect); // Suspend the task as it's not needed right now

  LOGGER("Setting up Web Server Paths & Commands...", INFO);

  for (uint8_t i = 0; i < sizeof(g_strWebServerFiles) / sizeof(g_strWebServerFiles[0]); i++) {
    String path = "/" + String(g_strWebServerFiles[i]);

    pWebServer.serveStatic(path.c_str(), SD, path.c_str()).setCacheControl("max-age=86400");
  }

  pWebServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    IPAddress pClientIP = request->client()->getRemoteAddress();

    //LOGGER("HTTP Request IP From: %d.%d.%d.%d.", INFO, pClientIP[0], pClientIP[1], pClientIP[2], pClientIP[3]);

    if (request->hasArg("action")) {  // Process the request
      if (request->arg("action") == "sync") {  // This is for update Elements from the Panel, serialization is: Element_ID:Value
        String strReturn = "";

        if (request->hasArg("watering")) {  // Update Web Panel Watering Needed Soil Humidity Or Elapsed Time. Based un Watering Criteria
          strReturn += "wathum:";

          String strValues = request->arg("watering");
          uint8_t nDivIndex = strValues.indexOf('|');

          if (strValues.substring(0, nDivIndex) == "0") // 0 equal to Humidity Criteria
            strReturn += String(g_pSettings[strValues.substring(nDivIndex + 1).toInt()].StartWateringHumidity);
          else
            strReturn += String(g_pSettings[strValues.substring(nDivIndex + 1).toInt()].StartWateringInterval / 3600000);
        } else if (request->hasArg("profilesel")) {
          uint8_t nSelectedProfile = request->arg("profilesel").toInt();
          ///////////////////////////////////////////////////
          strReturn += "lightstart:" + String(g_pSettings[nSelectedProfile].StartLightTime);
          strReturn += ":lightstop:" + String(g_pSettings[nSelectedProfile].StopLightTime);
          strReturn += ":lightbright:" + String(g_pSettings[nSelectedProfile].LightBrightness);
          ///////////////////////////////////////////////////
          strReturn += ":intfansta:" + String(g_pSettings[nSelectedProfile].StartInternalFanTemperature);
          ///////////////////////////////////////////////////
          strReturn += ":venttempstart:" + String(g_pSettings[nSelectedProfile].StartVentilationTemperature);
          strReturn += ":venthumstart:" + String(g_pSettings[nSelectedProfile].StartVentilationHumidity);
          ///////////////////////////////////////////////////
          strReturn += ":wateringmode:" + String(g_pSettings[nSelectedProfile].WateringCriteria == 0 ? "" : "checked");

          if (g_pSettings[nSelectedProfile].WateringCriteria == 0)
            strReturn += ":wathum:" + String(g_pSettings[nSelectedProfile].StartWateringHumidity);
          else
            strReturn += ":wathum:" + String(g_pSettings[nSelectedProfile].StartWateringInterval / 3600000);

          strReturn += ":wateringtime:" + String(g_pSettings[nSelectedProfile].WateringDuration);
        }

        if (strReturn != "")
          request->send(200, "text/plain", "SYNC" + strReturn);
      } else if (request->arg("action") == "update") {  // This is for update Settings
        String strReturn;
        uint32_t nNewValue;
        uint8_t nProfile = g_nCurrentProfile;
        // =============== Current DateTime =============== //
        if (request->hasArg("settime")) {
          struct timeval tv;
          tv.tv_sec = request->arg("settime").toInt();
          tv.tv_usec = 0;

          settimeofday(&tv, nullptr);

          configTime(TIMEZONE_UTC_OFFSET, TIMEZONE_DST_OFFSET, nullptr, nullptr);

          time_t timeNow = time(nullptr);
          struct tm timeInfo;

          localtime_r(&timeNow, &timeInfo);

          strReturn += "\r\nSe actualizó la Fecha actual.";
        }
        // =============== Profile Selector =============== //
        if (request->hasArg("profilesel")) {
          nNewValue = request->arg("profilesel").toInt();

          if (nNewValue != g_nCurrentProfile) {
            g_nCurrentProfile = nNewValue;
            nProfile = nNewValue;

            ProfilesLoader();

            strReturn += "\r\nSe cambió al Perfil de: ";
            strReturn += (nNewValue == 0) ? "Vegetativo." : ((nNewValue == 1) ? "Floración." : "Secado.");
          }
        }
        // =============== Light Start =============== //
        if (request->hasArg("lightstart")) {
          nNewValue = request->arg("lightstart").toInt();

          if (nNewValue != g_pSettings[g_nCurrentProfile].StartLightTime) {
            g_pSettings[g_nCurrentProfile].StartLightTime = nNewValue;
            g_nEffectiveStartLights = (g_pSettings[g_nCurrentProfile].StartLightTime == 24) ? 0 : g_pSettings[g_nCurrentProfile].StartLightTime;  // Stores the effective light start hour, converting 24 to 0 (midnight)

            strReturn += "\r\nSe actualizó la Hora de Encendido de Luz.";
          }
        }
        // =============== Light Stop =============== //
        if (request->hasArg("lightstop")) {
          nNewValue = request->arg("lightstop").toInt();

          if (nNewValue != g_pSettings[g_nCurrentProfile].StopLightTime) {
            g_pSettings[g_nCurrentProfile].StopLightTime = nNewValue;
            g_nEffectiveStopLights = (g_pSettings[g_nCurrentProfile].StopLightTime == 0) ? 24 : g_pSettings[g_nCurrentProfile].StopLightTime; // Stores the effective light stop hour, converting 0 to 24 (midnight)

            strReturn += "\r\nSe actualizó la Hora de Apagado de Luz.";
          }
        }
        // =============== Light Brightness =============== //
        if (request->hasArg("lightbright")) {
          String strValues = request->arg("lightbright");
          uint8_t nDivIndex = strValues.indexOf('|');
          uint8_t nParsedProfile = strValues.substring(0, nDivIndex).toInt();

          nNewValue = strValues.substring(nDivIndex + 1).toInt();

          if (request->args() == 2) // Just updating lightbright
            nProfile = nParsedProfile;
          else  // ¿Probably updating all values?
            nNewValue = request->arg("lightbright").toInt();

          if (nNewValue != g_pSettings[nProfile].LightBrightness) {
            g_pSettings[nProfile].LightBrightness = nNewValue < 401 ? 0 : nNewValue;

            ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - g_pSettings[nProfile].LightBrightness);

            strReturn += "\r\nSe actualizó la Intensidad de Luz.";
          }
        }
        // =============== Internal Fan Start =============== //
        if (request->hasArg("intfansta")) {
          nNewValue = request->arg("intfansta").toInt();

          if (nNewValue != g_pSettings[g_nCurrentProfile].StartInternalFanTemperature) {
            g_pSettings[g_nCurrentProfile].StartInternalFanTemperature = nNewValue;

            strReturn += "\r\nSe actualizó la Temperatura de Encendido del Ventilador Interno.";
          }
        }
        // =============== Ventilation Temperature Start =============== //
        if (request->hasArg("venttempstart")) {
          nNewValue = request->arg("venttempstart").toInt();

          if (nNewValue != g_pSettings[g_nCurrentProfile].StartVentilationTemperature) {
            g_pSettings[g_nCurrentProfile].StartVentilationTemperature = nNewValue;

            strReturn += "\r\nSe actualizó la Temperatura de Encendido de Recirculación.";
          }
        }
        // =============== Ventilation Humidity Start =============== //
        if (request->hasArg("venthumstart")) {
          nNewValue = request->arg("venthumstart").toInt();

          if (nNewValue != g_pSettings[g_nCurrentProfile].StartVentilationHumidity) {
            g_pSettings[g_nCurrentProfile].StartVentilationHumidity = nNewValue;

            strReturn += "\r\nSe actualizó la Humedad de Encendido de la Recirculación.";
          }
        }
        // =============== Watering Mode =============== //
        if (request->hasArg("wateringmode")) {
          nNewValue = request->arg("wateringmode").toInt();

          if (nNewValue != g_pSettings[g_nCurrentProfile].WateringCriteria) {
            g_pSettings[g_nCurrentProfile].WateringCriteria = nNewValue;

            strReturn += "\r\nSe actualizó el Criterio de Riego.";
          }
        }
        // =============== Soil Humidity Or Time Intervals Start Watering =============== //
        if (request->hasArg("wathum")) {
          nNewValue = request->arg("wathum").toInt();

          if (g_pSettings[g_nCurrentProfile].WateringCriteria == 0) { // Watering by Soil Moisture
            if (nNewValue != g_pSettings[g_nCurrentProfile].StartWateringHumidity) {
              g_pSettings[g_nCurrentProfile].StartWateringHumidity = nNewValue;

              strReturn += "\r\nSe actualizó el Nivel de Humedad mínimo para Regar.";
            }
          } else {  // Watering By Intervals
            nNewValue *= 3600000;

            if (nNewValue != g_pSettings[g_nCurrentProfile].StartWateringInterval) {
              g_pSettings[g_nCurrentProfile].StartWateringInterval = nNewValue;

              strReturn += "\r\nSe actualizó el Intervalo de Riego.";
            }
          }
        }
        // =============== Watering Time =============== //
        if (request->hasArg("wateringtime")) {
          nNewValue = request->arg("wateringtime").toInt();

          if (nNewValue != g_pSettings[g_nCurrentProfile].WateringDuration) {
            g_pSettings[g_nCurrentProfile].WateringDuration = nNewValue;

            strReturn += "\r\nSe actualizó la Duración de Riego.";
          }
        }
        // =============== Fans Rest Interval =============== //
        if (request->hasArg("restinterval")) {
          nNewValue = request->arg("restinterval").toInt();

          if (nNewValue != g_nFansRestInterval) {
            g_nFansRestInterval = nNewValue;

            strReturn += "\r\nSe actualizó el Intervalo de Reposo de Ventiladores.";
          }
        }
        // =============== Fans Rest Duration =============== //
        if (request->hasArg("restdur")) {
          nNewValue = request->arg("restdur").toInt();

          if (nNewValue != g_nFansRestDuration) {
            g_nFansRestDuration = nNewValue;

            strReturn += "\r\nSe actualizó la Duración de Reposo de Ventiladores.";
          }
        }
        // =============== Soil Humidity Read Interval =============== //
        if (request->hasArg("soilreadinterval")) {
          nNewValue = request->arg("soilreadinterval").toInt() * 1000;

          if (nNewValue != g_nSoilReadsInterval) {
            g_nSoilReadsInterval = nNewValue;

            strReturn += "\r\nSe actualizó el Intervalo de Lectura de Humedad del Suelo.";
          }
        }
        // =============== Environment Temperature Hysteresis =============== //
        if (request->hasArg("temphys")) {
          nNewValue = request->arg("temphys").toInt();

          if (nNewValue != g_nTemperatureStopHysteresis) {
            g_nTemperatureStopHysteresis = nNewValue;

            strReturn += "\r\nSe actualizó la Histéresis de Apagado por Temperatura.";
          }
        }
        // =============== Environment Humidity Hysteresis =============== //
        if (request->hasArg("humhys")) {
          nNewValue = request->arg("humhys").toInt();

          if (nNewValue != g_nHumidityStopHysteresis) {
            g_nHumidityStopHysteresis = nNewValue;

            strReturn += "\r\nSe actualizó la Histéresis de Apagado por Humedad.";
          }
        }
        // =============== Drip Per Minute =============== //
        if (request->hasArg("dripperminute")) {
          nNewValue = request->arg("dripperminute").toInt();

          if (nNewValue != g_nDripPerMinute) {
            g_nDripPerMinute = nNewValue;

            strReturn += "\r\nSe actualizó el valor de Tasa de Goteo por Minuto.";
          }
        }
        // =============== WiFi =============== //
        bool bWiFiChanges = false;

        if (request->hasArg("ssid") && strcmp(request->arg("ssid").c_str(), g_cSSID) != 0) {
          strncpy(g_cSSID, request->arg("ssid").c_str(), sizeof(g_cSSID) - 1);
          bWiFiChanges = true;

          strReturn += "\r\nSe actualizó el SSID de Wifi.";
        }

        if (request->hasArg("ssidpwd") && strcmp(request->arg("ssidpwd").c_str(), g_cSSIDPWD) != 0) {
          strncpy(g_cSSIDPWD, request->arg("ssidpwd").c_str(), sizeof(g_cSSIDPWD) - 1);
          bWiFiChanges = true;

          strReturn += "\r\nSe actualizó la Contraseña de Wifi.";
        }

        if (bWiFiChanges) {
#ifdef ENABLE_AP_ALWAYS
          strReturn += "\r\nSe intentará conectar a la nueva Red.";
#else
          strReturn += "\r\nSe intentará conectar a la nueva Red, de no ser posible; se iniciará una Red Wifi (" + String(ACCESSPOINT_NAME) + ") para poder reconfigurar el controlador.";
#endif
        }

        if (strReturn != "") {  // If have some change, send response to web client and finally save new settings values
          request->send(200, "text/plain", "MSG" + strReturn);

          ConnectSD(false);

          if (g_bIsSDInit) {
            nProfile = (nProfile != g_nCurrentProfile ? nProfile : g_nCurrentProfile);  // I reuse this variable

            File pSettingsFile = SD.open("/settings", FILE_WRITE);  // Save Internal Settings
            if (pSettingsFile) {
              pSettingsFile.println(g_cSSID);
              pSettingsFile.println(g_cSSIDPWD);

              pSettingsFile.println(nProfile);

              pSettingsFile.println(g_nFansRestInterval);
              pSettingsFile.println(g_nFansRestDuration);

              pSettingsFile.println(g_nSoilReadsInterval);

              pSettingsFile.println(g_nTemperatureStopHysteresis);
              pSettingsFile.println(g_nHumidityStopHysteresis);

              pSettingsFile.println(g_nDripPerMinute);

              LOGGER("Settings file updated successfully.", INFO);
            }

            pSettingsFile.close();
            ///////////////////////////////////////////////////
            String strProfileName = ((nProfile == 0) ? "/veg" : ((nProfile == 1) ? "/flo" : "/dry"));
            File pProfileFile = SD.open(strProfileName, FILE_WRITE);  // Save Current Profile Values
            if (pProfileFile) {
              pProfileFile.println(g_pSettings[nProfile].StartLightTime);
              pProfileFile.println(g_pSettings[nProfile].StopLightTime);
              pProfileFile.println(g_pSettings[nProfile].LightBrightness);
              pProfileFile.println(g_pSettings[nProfile].StartInternalFanTemperature);
              pProfileFile.println(g_pSettings[nProfile].StartVentilationTemperature);
              pProfileFile.println(g_pSettings[nProfile].StartVentilationHumidity);
              pProfileFile.println(g_pSettings[nProfile].WateringCriteria);
              pProfileFile.println(g_pSettings[nProfile].StartWateringInterval);
              pProfileFile.println(g_pSettings[nProfile].StartWateringHumidity);
              pProfileFile.println(g_pSettings[nProfile].WateringDuration);

              LOGGER("Profile: %s updated successfully.", INFO, strProfileName);
            }

            pProfileFile.close();
          }
        }

        // After send response to web client, Try update & reconnect to Wifi if is required
        if (bWiFiChanges) {
          WiFi.disconnect(false); // First disconnect from current Network (Arg false to just disconnect the Station, not the AP)

          if (eTaskGetState(pWiFiReconnect) == eRunning)  // Then check if task pWiFiReconnect is running. If it is, Suspend it to eventually Resume it with the new SSID and SSID Password
            vTaskSuspend(pWiFiReconnect);
        }
      } else if (request->arg("action") == "refresh") { // This is for refresh Panel values
        String strResponse = "";
        // ================================================== Environment Section ================================================== //
        strResponse += String(g_nEnvironmentTemperature) + ":" + String(g_nEnvironmentHumidity) + ":" + String(g_fEnvironmentVPD, 2);
        // ================================================== Soil Section ================================================== //
        strResponse += ":" + String(nSoilHumidityPinsCount);

        for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++)
          strResponse += ":" + String(g_nSoilsHumidity[i]);
        // ================================================== Watering Section ================================================== //
        strResponse += ":" + String(g_nWateringElapsedTime);
        // ================================================== Current Time Section ================================================== //
        time_t timeNow = time(nullptr);

        strResponse += ":" + String(timeNow);
        // ================================================== Light Brightness slider position Section ================================================== //
        uint8_t nProfile = request->arg("profile").toInt();
        strResponse += ":" + String(g_pSettings[nProfile].LightBrightness < 401 ? 0 : g_pSettings[nProfile].LightBrightness);
        // ================================================== Fans Rest State Section ================================================== //
        strResponse += ":";

        if (g_bFansRest)
          strResponse += String((g_nFansRestDuration * 60000) - (millis() - g_ulFansRestElapsedTime));
        else
          strResponse += "0";

        // Internal Fan
        strResponse += ":" + String(digitalRead(GetPinByName("Internal Fan"))); // HIGHT=1 A.K.A paused

        // Ventilation
        strResponse += ":" + String(digitalRead(GetPinByName("Ventilation Fan")));  // HIGHT=1 A.K.A paused
        // ================================================== Graph Section ================================================== //
        ConnectSD(false);

        if (g_bIsSDInit) {  // V2 stolen code
          static size_t sizeLastReadFile = 0;

          File pFile = SD.open("/metrics.log", FILE_READ);
          if (pFile) {
            size_t sizeFile = pFile.size();

            if (sizeFile > sizeLastReadFile) {  // Don't read the file if size not has been changed
              sizeLastReadFile = sizeFile;
              const size_t sizeBlock = 512;
              char cChunkBuffer[sizeBlock];
              int64_t nPos = sizeFile - sizeBlock;
              uint8_t nReadedLines = 0;

              while (nPos >= 0 && nReadedLines < MAX_GRAPH_MARKS) {
                pFile.seek(nPos);

                size_t bytesRead = pFile.read((uint8_t*)cChunkBuffer, (nPos < sizeBlock) ? nPos + 1 : sizeBlock);
                if (bytesRead == 0)
                  break;

                for (int i = bytesRead - 1; i >= 0; i--) {
                  if (cChunkBuffer[i] == '\n' || (nPos == 0 && i == 0)) {
                    pFile.seek(nPos + i + 1);

                    char cBuffer[64];
                    uint8_t nBytesRead = pFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1);
                    cBuffer[nBytesRead] = '\0';

                    if (nBytesRead > 0) {
                      while (cBuffer[nBytesRead - 1] == '\r' || cBuffer[nBytesRead - 1] == ' ')
                        cBuffer[--nBytesRead] = '\0';

                      if (g_strArrayGraphData[nReadedLines] != nullptr)
                        free((void*)g_strArrayGraphData[nReadedLines]);

                      g_strArrayGraphData[nReadedLines] = strdup(cBuffer);

                      nReadedLines++;
                    }

                    if (nReadedLines >= MAX_GRAPH_MARKS)
                      break;
                  }
                }

                if (nReadedLines >= MAX_GRAPH_MARKS || nPos == 0)
                  break;

                nPos -= sizeBlock;
                if (nPos < 0)
                  nPos = 0;
              }
            }

            pFile.close();
          }
        }

        size_t sizeGraphArraySize = sizeof(g_strArrayGraphData) / sizeof(g_strArrayGraphData[0]);

        if (sizeGraphArraySize > 0 && g_strArrayGraphData[0] != nullptr) {
          strResponse += ":";

          for (int i = sizeGraphArraySize - 1; i >= 0; i--) {
            if (g_strArrayGraphData[i] != nullptr) {
              if (i < sizeGraphArraySize - 1)
                strResponse += ",";

              strResponse += String(g_strArrayGraphData[i]);
            }
          }
        }
        // ========================================================================================================================= //
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain;charset=utf-8", "REFRESH" + strResponse);

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
    } else {  // Return Panel content
      ConnectSD(false);

      AsyncWebServerResponse *pResponse;

      if (g_bIsSDInit)
        pResponse = request->beginResponse(SD, "/index.html", "text/html", false, HTMLProcessor);
      else
        pResponse = request->beginResponse(500, "text/plain", "No hay una Tarjeta SD conectada.");

      request->send(pResponse);
    }
  });

  pWebServer.begin();

  LOGGER("Web Server Started at Port: %d.", INFO, WEBSERVER_PORT);
}

void loop() {
  static unsigned long ulLastSecondTick = 0;
  unsigned long ulCurrentMillis = millis(); 

  if ((ulCurrentMillis - ulLastSecondTick) >= 1000) { // Check if 1 second has passed since the last tick to perform once-per-second tasks
    ulLastSecondTick = ulCurrentMillis;

    time_t timeNow = time(nullptr);
    struct tm *timeInfo = localtime(&timeNow);
    // ================================================== Wifi Section ================================================== //
    if (ulCurrentMillis >= (g_ulStartUpTime + (1000 /*Delay between wifi connect check un Setup()*/ * WIFI_MAX_RETRYS)) && WiFi.status() != WL_CONNECTED && eTaskGetState(pWiFiReconnect) == eSuspended) // If is not connected to Wifi and is not currently running a reconnect trask, start it
      vTaskResume(pWiFiReconnect);
    // ================================================== Time Section ================================================== //
    WriteToSD("/time", String((uint32_t)timeNow), false); // Write current time to SD Card
    // ================================================== Environment Section ================================================== //
    GetEnvironmentParameters(g_nEnvironmentTemperature, g_nEnvironmentHumidity, g_fEnvironmentVPD); // Get environment parameters to store in Global variables
    // ================================================== Soil Section ================================================== //
    if ((ulCurrentMillis - g_ulSoilHumidityElapsedReadTime) >= g_nSoilReadsInterval) {
      g_ulSoilHumidityElapsedReadTime = ulCurrentMillis;

      LOGGER("Environment Temperature: %d°C Humidity: %d%% VPD: %.2fkPa.", INFO, g_nEnvironmentTemperature, g_nEnvironmentHumidity, g_fEnvironmentVPD);

      for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++) {
        g_nSoilsHumidity[i] = GetSoilHumidity(i);

        LOGGER("Soil Sensor %d Humidity: %d%%.", INFO, i, g_nSoilsHumidity[i]);
      }
    }
    // ================================================== Lights Section ================================================== //
    if ((g_pSettings[g_nCurrentProfile].StartLightTime > 0 || g_pSettings[g_nCurrentProfile].StopLightTime > 0) && (g_nEffectiveStartLights < g_nEffectiveStopLights &&
          timeInfo->tm_hour >= g_nEffectiveStartLights && timeInfo->tm_hour < g_nEffectiveStopLights)  // If the Start hour is earlier than the Stop hour (e.g., from 8 to 20), turn on the lights only if the current hour is within that range
      || (g_nEffectiveStartLights >= g_nEffectiveStopLights &&
            (timeInfo->tm_hour >= g_nEffectiveStartLights || timeInfo->tm_hour < g_nEffectiveStopLights))) {  // If the Start hour is later than or equal to the Stop hour (e.g., from 20 to 6), turn on the Lights if the current hour is either after the Start or before the Stop
      if (digitalRead(GetPinByName("Lights"))) {
        digitalWrite(GetPinByName("Lights"), LOW);

        LOGGER("Lights Started.", INFO);
      }
    } else {  // If Current Time is out of ON range, turn it OFF
      if (!digitalRead(GetPinByName("Lights"))) {
        digitalWrite(GetPinByName("Lights"), HIGH);

        LOGGER("Lights Stopped.", INFO);
      }
    }
    // ================================================== Fans Section ================================================== //
    if (!g_bFansRest) { // If is not rest time for Fans
      if (g_nEnvironmentTemperature > 0 && g_nEnvironmentHumidity > 0) {  // Check if DHT11 Sensor is working
        // ================================================== Internal Fan control by Temperature ================================================== //
        if (g_nEnvironmentTemperature >= g_pSettings[g_nCurrentProfile].StartInternalFanTemperature) {  // If Environment Temperature if higher or equal to needed to Start Internal Fan Temperature, start it
          if (digitalRead(GetPinByName("Internal Fan"))) {
            digitalWrite(GetPinByName("Internal Fan"), LOW);

            LOGGER("Internal Fan Started.", INFO);
          }
        } else if (g_nEnvironmentTemperature <= g_pSettings[g_nCurrentProfile].StartInternalFanTemperature - g_nTemperatureStopHysteresis) {  // If Environment Temperature is less or equal to: Start Internal Fan Temperature - Temperature Hysteresis, stop it
          if (!digitalRead(GetPinByName("Internal Fan"))) {
            digitalWrite(GetPinByName("Internal Fan"), HIGH);

            LOGGER("Internal Fan Stopped.", INFO);
          }
        }
        // ================================================== Ventilation Fans control by Temperature & Humidity ================================================== //
        static bool bStartVentilationByTemperature = false;
        static bool bStartVentilationByHumidity = false;
        // Control by Temperature
        if (g_nEnvironmentTemperature >= g_pSettings[g_nCurrentProfile].StartVentilationTemperature)
          bStartVentilationByTemperature = true;
        else if (g_nEnvironmentTemperature <= g_pSettings[g_nCurrentProfile].StartVentilationTemperature - g_nTemperatureStopHysteresis)
          bStartVentilationByTemperature = false;
        // Control by Humidity
        if (g_nEnvironmentHumidity >= g_pSettings[g_nCurrentProfile].StartVentilationHumidity)
          bStartVentilationByHumidity = true;
        else if (g_nEnvironmentHumidity <= g_pSettings[g_nCurrentProfile].StartVentilationHumidity - g_nHumidityStopHysteresis)
          bStartVentilationByHumidity = false;
        ///////////////////////////////////////////////////
        if (bStartVentilationByTemperature || bStartVentilationByHumidity) {
          if (digitalRead(GetPinByName("Ventilation Fan"))) {
              digitalWrite(GetPinByName("Ventilation Fan"), LOW);

              LOGGER("Ventilation Started.", INFO);
            }
        } else {
          if (!digitalRead(GetPinByName("Ventilation Fan"))) {
            digitalWrite(GetPinByName("Ventilation Fan"), HIGH);

            LOGGER("Ventilation Stopped.", INFO);
          }
        }
      }
    }
    // ================================================== Watering Section ================================================== //
    if (digitalRead(GetPinByName("Water Pump")) && (g_pSettings[g_nCurrentProfile].StartWateringHumidity > 0 || g_pSettings[g_nCurrentProfile].StartWateringInterval > 0)) {
      if ((ulCurrentMillis - g_ulStartUpTime) >= (g_nSoilReadsInterval * 1.5)) { // WARNING: Hardcode this is to prevent prematurely watering
        bool bStartWatering = false;

        if (g_pSettings[g_nCurrentProfile].WateringCriteria == 0) { // Watering by Soil Moisture
          uint16_t nCombinedHumidity = 0;

          for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++)
            nCombinedHumidity += g_nSoilsHumidity[i];

          if ((nCombinedHumidity / nSoilHumidityPinsCount) <= g_pSettings[g_nCurrentProfile].StartWateringHumidity) {
            bStartWatering = true;

            LOGGER("Watering Started by Soil Humidity.", INFO);
          }
        } else {  // Watering by Elapsed Time
          if ((ulCurrentMillis - g_ulWateringIntervalElapsedTime) >= g_pSettings[g_nCurrentProfile].StartWateringInterval) {
            g_ulWateringIntervalElapsedTime = ulCurrentMillis;
            bStartWatering = true;

            LOGGER("Watering Started by Interval.", INFO);
          }
        }
        ///////////////////////////////////////////////////
        if (bStartWatering) {
          g_nWateringElapsedTime = 0; // Restard the watering elapsed time counter

          digitalWrite(GetPinByName("Water Pump"), LOW);
        }
      }

      if (!digitalRead(GetPinByName("Water Pump"))) {
        if (g_nWateringElapsedTime >= g_pSettings[g_nCurrentProfile].WateringDuration) {  // If Watering time elapsed is more or equal to Watering Duration
          for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++)
            g_nSoilsHumidity[i] = GetSoilHumidity(i); // Update the soils moisture values

          g_nWateringElapsedTime = 0;  // Restard the watering elapsed time counter

          digitalWrite(GetPinByName("Water Pump"), HIGH); // And stop the Pump

          LOGGER("Stopped Watering.", INFO);
        } else {  // If is in Watering time
          g_nWateringElapsedTime++; // Increase the counter
        }
      }
    }
    // ================================================== Store Data for Graph Section ================================================== //
    if ((ulCurrentMillis - g_ulStartUpTime) >= (g_nSoilReadsInterval + 60000) /*+1 Minute*/ && (ulCurrentMillis - g_ulLastStoreElapsedTime) >= GRAPH_MARKS_INTERVAL) {  // Checks if at least one minute has passed since soil read interval and GRAPH_MARKS_INTERVAL since last data store
      g_ulLastStoreElapsedTime = ulCurrentMillis;

      String strValues = String(timeNow) + "|" + String(g_nEnvironmentTemperature) + "|" + String(g_nEnvironmentHumidity) + "|" + String(g_fEnvironmentVPD, 2);

      for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++)
       strValues += "|" + String(g_nSoilsHumidity[i]);

      WriteToSD("/metrics.log", strValues, true);
    }
    // ================================================== Fans Rest Time Section ================================================== //
    static unsigned long ulFansRestIntervalTime = 0;

    if (ulCurrentMillis - ulFansRestIntervalTime >= g_nFansRestInterval * 60000) {
      ulFansRestIntervalTime = ulCurrentMillis;
      g_ulFansRestElapsedTime = ulCurrentMillis;
      g_bFansRest = true;

      digitalWrite(GetPinByName("Internal Fan"), HIGH);
      digitalWrite(GetPinByName("Ventilation Fan"), HIGH);

      LOGGER("Fans Rest mode started.", INFO);
    }
    ///////////////////////////////////////////////////
    if (g_bFansRest) {
      if ((ulCurrentMillis - g_ulFansRestElapsedTime) >= (g_nFansRestDuration * 60000)) {
        g_bFansRest = false;
        g_ulFansRestElapsedTime = 0;

        LOGGER("Fans Rest mode completed.", INFO);
      }
    }
  }
}
