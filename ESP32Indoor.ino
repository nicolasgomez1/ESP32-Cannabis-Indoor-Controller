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
//                               Version 3 (2025)                                //
///////////////////////////////////////////////////////////////////////////////////
#include <SD.h> // https://docs.arduino.cc/libraries/sd/#SD%20class
#include <WiFi.h>
#include <time.h>
#include <ESPmDNS.h>
#include <SimpleDHT.h>
#include <ESPAsyncWebServer.h>
#include <vector>

struct RelayPin {
  const char* Name; // Channel name
  uint8_t Pin;      // Pin number
};

struct WateringData {
  uint8_t Day;
  uint16_t TargetCC;

  bool operator==(const WateringData& other) const {  // Little overload but needed for compare
    return Day == other.Day && TargetCC == other.TargetCC;
  }
};

struct Settings {
  uint8_t StartLightTime;
  uint8_t StopLightTime;
  uint16_t LightBrightness;

  uint8_t StartInternalFanTemperature;

  uint8_t StartVentilationTemperature;
  uint8_t StartVentilationHumidity;

  uint16_t CurrentWateringDay;
  std::vector<WateringData> WateringStages;
};

// NOTES:
// Default IP for AP mode is: 192.168.4.1
// If Environment Humidity or Temperature Reads 0, the fans never gonna start.
// If Light Start & Stop Times Is 0, the light never gonna start.
// DHT1 have a pullup (between data and vcc)
// HW080 have a pulldown (in return line to gnd)

// Definitions
//#define ENABLE_SERIAL_LOGGER  // Use this when debugging
//#define ENABLE_SD_LOGGING   // Use this to save logs to SD Card
//#define ENABLE_AP_ALWAYS      // Use this to enable always the Access Point. Else it just enable when have no internet connection

#define MAX_PROFILES 3  // 0 Vegetative (Filename: veg), 1 Flowering (Filename: flo), 2 Drying (Filename: dry)

#define MAX_GRAPH_MARKS 48  // How much logs show in Web Panel Graph
#define GRAPH_MARKS_INTERVAL 3600000/*1 Hour*/  // Intervals in which values ​​are stored for the graph // TODO: Esto lo podría incluir en la configuración interna. Para poder cambiarlo desde el Panel Web

#define WIFI_MAX_RETRYS 5 // Max Wifi reconnection attempts
#define WIFI_CHECK_INTERVAL 1000/*1 Second*/

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
  { "Ventilation", 26 },  // Pin for Channel 2 of Relay Module
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

// DO NOT TOUCH IT!
enum ERR_TYPE { INFO, WARN, ERROR };

#define PIN_ON  LOW
#define PIN_OFF HIGH

// Settings storage Variables
char g_cSSID[32];
char g_cSSIDPWD[32];

unsigned long g_ulFansRestInterval = 0;
unsigned long g_ulFansRestDuration = 0;

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
int8_t g_nLastResetDay = -1;
bool g_bWateredHour[24] = { false };
unsigned long g_ulWateringDuration = 0;
bool g_bTestPump = false;
unsigned long g_ulTestWateringPumpStartTime = 0;
uint8_t g_nEnvironmentTemperature = 0;
uint8_t g_nEnvironmentHumidity = 0;
float g_fEnvironmentVPD = 0.0f;
unsigned long g_ulFansRestElapsedTime = 0;
bool g_bFansRest = false;
uint8_t g_nSoilsHumidity[nSoilHumidityPinsCount] = {};
char* g_strArrayGraphData[MAX_GRAPH_MARKS] = {};

// Global Handles, Interface & Instances
AsyncWebServer pWebServer(WEBSERVER_PORT);  // Asynchronous web server instance listening on WEBSERVER_PORT
SimpleDHT11 pDHT11(DHT_DATA_PIN);           // Interface to DHT11 Temperature & Humidity sensor
TaskHandle_t pWiFiReconnect;                // Task handle for Wifi reconnect logic running on core 0

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

  if (!g_bIsSDInit)
    return;

  if (strFileName == "logging_") {
    time_t timeNow = time(nullptr);
    struct tm *timeInfo = localtime(&timeNow);

    strFileName += String(timeInfo->tm_year + 1900) + "_" + String(timeInfo->tm_mon + 1) + "_" + String(timeInfo->tm_mday) + ".log";
  }

  File pFile = SD.open(strFileName.c_str(), (bAppend ? FILE_APPEND : FILE_WRITE));
  if (pFile) {
    pFile.println(strText.c_str());

    pFile.close();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Saves the current configuration of the specified profile to the SD card.
// The profile index (0 = veg, 1 = flo, 2 = dry) determines the file path used.
// Writes general configuration values (lighting and climate settings) to a main file (e.g., /veg).
// Writes irrigation stage data (day and target cc values) to a separate file with "_watering" suffix (e.g., /veg_watering).
void WriteProfile(uint8_t nProfile) {
  ConnectSD(false);

  if (!g_bIsSDInit)
    return;

  String strProfileName = ((nProfile == 0) ? "/veg" : ((nProfile == 1) ? "/flo" : "/dry"));
  File pProfileFile = SD.open(strProfileName, FILE_WRITE);  // Save Current Profile Values
  if (pProfileFile) {
    pProfileFile.println(g_pSettings[nProfile].StartLightTime);
    pProfileFile.println(g_pSettings[nProfile].StopLightTime);
    pProfileFile.println(g_pSettings[nProfile].LightBrightness);
    pProfileFile.println(g_pSettings[nProfile].StartInternalFanTemperature);
    pProfileFile.println(g_pSettings[nProfile].StartVentilationTemperature);
    pProfileFile.println(g_pSettings[nProfile].StartVentilationHumidity);
    pProfileFile.println(g_pSettings[nProfile].CurrentWateringDay);

    pProfileFile.close();

    File pWateringProfileFile = SD.open(strProfileName + "_watering", FILE_WRITE);  // Save Current Profile Values
    if (pWateringProfileFile) {
      for (const auto& Watering : g_pSettings[nProfile].WateringStages)
        pWateringProfileFile.printf("%u|%u\n", Watering.Day, Watering.TargetCC);

      pWateringProfileFile.close();
    }

    LOGGER("Profile: %s updated successfully.", INFO, strProfileName);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Loads profile configuration data from SD card files (/veg, /flo, /dry) into the g_pSettings array.
// If the SD card is not initialized, attempts to initialize it by calling ConnectSD(false).
// For the current active profile (g_nCurrentProfile), updates effective light start/stop times
// by normalizing hour values (24 to 0 for start, 0 to 24 for stop).
void ProfilesLoader() {
  ConnectSD(false);

  if (!g_bIsSDInit)
    return;

  char cBuffer[64];

  for (uint8_t i = 0; i < MAX_PROFILES; i++) {
    String strProfileName = ((i == 0) ? "/veg" : ((i == 1) ? "/flo" : "/dry"));

    File pProfileFile = SD.open(strProfileName, FILE_READ);
    if (pProfileFile) {
      cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // START LIGHT TIME

      g_pSettings[i].StartLightTime = atoi(cBuffer);

      if (g_nCurrentProfile == i) // Only if the current loop corresponds to the current profile
        g_nEffectiveStartLights = (g_pSettings[g_nCurrentProfile].StartLightTime == 24) ? 0 : g_pSettings[g_nCurrentProfile].StartLightTime;  // Stores the effective light start hour, converting 24 to 0 (midnight)
      ///////////////////////////////////////////////////
      cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // STOP LIGHT TIME

      g_pSettings[i].StopLightTime = atoi(cBuffer);

      if (g_nCurrentProfile == i) // Only if the current loop corresponds to the current profile
        g_nEffectiveStopLights = (g_pSettings[g_nCurrentProfile].StopLightTime == 0) ? 24 : g_pSettings[g_nCurrentProfile].StopLightTime; // Stores the effective light stop hour, converting 0 to 24 (midnight)
      ///////////////////////////////////////////////////
      cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // LIGHT BRIGHTNESS LEVEL

      g_pSettings[i].LightBrightness = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // INTERNAL FAN TEMPERATURE START

      g_pSettings[i].StartInternalFanTemperature = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // VENTILATION TEMPERATURE START

      g_pSettings[i].StartVentilationTemperature = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // VENTILATION HUMIDITY START

      g_pSettings[i].StartVentilationHumidity = atoi(cBuffer);
      /////////////////////////////////////////////////// 
      cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // CURRENT WATERING DAY

      g_pSettings[i].CurrentWateringDay = atoi(cBuffer);

      pProfileFile.close();
    }

    File pWateringProfileFile = SD.open(strProfileName + "_watering", FILE_READ);
    if (pWateringProfileFile) {
      g_pSettings[i].WateringStages.clear();

      while (pWateringProfileFile.available()) {
        cBuffer[pWateringProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';
        TrimTrailingWhitespace(cBuffer);

        char* cDivider = strchr(cBuffer, '|');
        if (cDivider) {
          *cDivider = '\0';

          g_pSettings[i].WateringStages.push_back({atoi(cBuffer), atoi(cDivider + 1)});
        }
      }

      pWateringProfileFile.close();
    }
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
// Reads temperature and humidity from a DHT11 sensor, retrying a defined number of times if necessary.
// Updates global variables for temperature, humidity, and VPD. If the readings are invalid or fail after
// the maximum number of retries, logs an error and sets the global variables to default (0).
void GetEnvironmentParameters() {
  uint8_t nError = SimpleDHTErrSuccess;
  uint8_t nReadTrysCount = 0;
  byte bTemperature = 0, bHumidity = 0;

  do {  // Try read Temp & Humidity values from Environment
    if ((nError = pDHT11.read(&bTemperature, &bHumidity, NULL)) != SimpleDHTErrSuccess) { // Get Environment Temperature and Humidity
      nReadTrysCount++;

      //LOGGER("Read DHT11 failed, Error=%d, Duration=%d.", ERROR, SimpleDHTErrCode(nError), SimpleDHTErrDuration(nError));

      delay(100);
    }
  } while (nError != SimpleDHTErrSuccess && nReadTrysCount < DHT_MAX_READS);  // Repeat while it fails and not reach max trys

  if (nError == SimpleDHTErrSuccess) {
    g_nEnvironmentTemperature = (uint8_t)bTemperature;
    g_nEnvironmentHumidity = (uint8_t)bHumidity;

    if ((g_nEnvironmentTemperature > 0 && g_nEnvironmentTemperature <= 52) && (g_nEnvironmentHumidity > 0 && g_nEnvironmentHumidity <= 100)) {  // DHT11 → Temp 0~50±2 | Hum 0~80±5
      float E_s = 6.112 * exp((17.67 * g_nEnvironmentTemperature) / (243.5 + g_nEnvironmentTemperature));

      g_fEnvironmentVPD = (E_s - (g_nEnvironmentHumidity / 100.0) * E_s) / 10.0;
    } else {
      g_fEnvironmentVPD = 0;

      //LOGGER("Invalid Temperature or Humidity readings.", ERROR);
    }
  } else {
    g_nEnvironmentTemperature = 0;
    g_nEnvironmentHumidity = 0;
    g_fEnvironmentVPD = 0;

    //LOGGER("Failed to read DHT11 after max retries.", ERROR);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads the average humidity value from a soil sensor. 
// Excites the sensor by setting the VCC pin high and waits for stable readings. 
// Takes multiple analog readings, sums them, and normalizes the result to a percentage scale (0-100). 
// After readings, the VCC pin is set low to stop electrolysis. 
// Returns the normalized humidity value as an integer between 0 and 100.
uint8_t GetSoilHumidity(uint8_t nSensorNumber) {
  digitalWrite(HW080_VCC_PIN, PIN_OFF);  // Put Pin output in High to excite the moisture sensors

  delay(10);  // Small Wait to obtain an stable reading

  unsigned long ulCombinedValues = 0;

  for (uint8_t i = 0; i < HW080_MAX_READS; i++) {
    ulCombinedValues += analogRead(nSoilHumidityPins[nSensorNumber]);
    delay(100); // Small delay between reads
  }

  digitalWrite(HW080_VCC_PIN, PIN_ON); // Put pin output in low to stop electrolysis

  return constrain(map(ulCombinedValues / HW080_MAX_READS, HW080_MIN, HW080_MAX, 0, 100), 0, 100);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Searches for a relay channel by its name.
// Returns the associated pin number if found.
// Returns -1 if the name does not match any configured relay.
uint8_t GetPinByName(const char* Name) {
    for (uint8_t i = 0; i < nRelayModulePinsCount; ++i) {
      if (strcmp(pRelayModulePins[i].Name, Name) == 0)
        return pRelayModulePins[i].Pin;
    }

    return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Removes trailing whitespace characters (spaces, tabs, newlines, etc.) from the end of the given null-terminated string.
// Modifies the input string in place.
void TrimTrailingWhitespace(char* str) {
  uint16_t nLen = strlen(str);

  while (nLen > 0 && isspace(str[nLen - 1]))
    str[--nLen] = '\0';
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Provides utility functions to convert between ticks (milliseconds) and human-readable time units.
// Ticks are assumed to be in milliseconds, as returned by the millis() function.
//unsigned long TicksToSeconds(unsigned long lTicks) { return lTicks / 1000; }
unsigned long TicksToMinutes(unsigned long lTicks) { return lTicks / (1000 * 60); }
//unsigned long TicksToHours(unsigned long lTicks) { return lTicks / (1000 * 60 * 60); }

//unsigned long SecondsToTicks(unsigned long lSeconds) { return lSeconds * 1000; }
unsigned long MinutesToTicks(unsigned long lMinutes) { return lMinutes * 1000 * 60; }
//unsigned long HoursToTicks(unsigned long lHours) { return lHours * 1000 * 60 * 60; }

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Handles automatic WiFi reconnection using the global SSID and password credentials.
// If ENABLE_AP_ALWAYS is not defined:
//   - Starts a temporary Access Point (ACCESSPOINT_NAME) to allow reconfiguration during reconnection attempts.
//   - Once connected, the Access Point is shut down and the mode is switched to station-only.
// Tries to reconnect up to WIFI_MAX_RETRYS times, with a 1-second delay between attempts.
// Logs a success message with IP address upon connection, or an error message if all attempts fail.
// After completion (regardless of success), the task is suspended until explicitly resumed elsewhere.
void Thread_WifiReconnect(void *parameter) {
  for (;;) {
#if !defined(ENABLE_AP_ALWAYS)
    if (!(WiFi.getMode() & WIFI_AP)) {
      LOGGER("Starting Access Point (SSID: %s) mode for reconfiguration...", INFO, ACCESSPOINT_NAME);

      WiFi.mode(WIFI_AP_STA); // Set dual mode, Access Point & Station

      vTaskDelay(100 / portTICK_PERIOD_MS); // Delay to stabilize AP

      WiFi.softAP(ACCESSPOINT_NAME); // Start Access Point, while try to connect to Wifi
    }
#endif

    LOGGER("Trying to reconnect Wifi...", INFO);

    WiFi.begin(g_cSSID, g_cSSIDPWD);

    uint8_t nConnectTrysCount = 0;

    while (WiFi.status() != WL_CONNECTED && nConnectTrysCount < WIFI_MAX_RETRYS) {
      nConnectTrysCount++;
      vTaskDelay(WIFI_CHECK_INTERVAL / portTICK_PERIOD_MS);  // Wait before trying again
    }

    if (WiFi.status() == WL_CONNECTED) {
      LOGGER("Connected to Wifi SSID: %s PASSWORD: %s. IP: %s.", INFO, g_cSSID, g_cSSIDPWD, WiFi.localIP().toString().c_str());

#if !defined(ENABLE_AP_ALWAYS)
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);

      LOGGER("Access Point disconnected.", INFO);
#endif
    } else {
      LOGGER("Max Wifi reconnect attempts reached.", ERROR);
    }

    vTaskSuspend(NULL); // Suspends the task until needed again
  }
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

    for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++)
      strReturn += "<tr><td>Humedad de Maceta " + String(i) + ":</td><td><p id=soil" + String(i) + ">" + String(g_nSoilsHumidity[i]) + "&#37;</p></td></tr>";

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
  } else if (var == "TIMEZONEUTCOFFSET") {
    return String(TIMEZONE_UTC_OFFSET);
  } else if (var == "BRIGHTLEVEL") {
    return String(g_pSettings[g_nCurrentProfile].LightBrightness < 401 ? 0 : g_pSettings[g_nCurrentProfile].LightBrightness); // WARNING: Hardcode offset
  } else if (var == "STAINTFAN") {
    return String(g_pSettings[g_nCurrentProfile].StartInternalFanTemperature);
  } else if (var == "TEMPSTARTVENT") {
    return String(g_pSettings[g_nCurrentProfile].StartVentilationTemperature);
  } else if (var == "HUMSTARTVENT") {
    return String(g_pSettings[g_nCurrentProfile].StartVentilationHumidity);
  } else if (var == "CURRENTWATERINGDAY") {
    return String(g_pSettings[g_nCurrentProfile].CurrentWateringDay);
  } else if (var == "WATSTATE") {
    String strReturn;

    if (g_ulWateringDuration > 0) {
      strReturn = "Regando...<br>Tiempo Restante: ";

      if (g_ulWateringDuration < 60) {
        strReturn += String(g_ulWateringDuration) + " segundos";
      } else {
        unsigned long ulMinutes = g_ulWateringDuration / 60;
        unsigned long ulSeconds = g_ulWateringDuration % 60;

        strReturn += String(ulMinutes) + (ulMinutes == 1 ? " minuto" : " minutos");

        if (ulSeconds > 0)
          strReturn += " y " + String(ulSeconds) + " segundos";
      }
    }

    return strReturn;
  } else if (var == "RESTSTATE") {
    String strReturn;

    if (g_ulFansRestElapsedTime > 0) {
      strReturn = "En Reposo...<br>Tiempo Restante: ";

      unsigned long ulTimeRemaining = g_ulFansRestDuration - (millis() - g_ulFansRestElapsedTime);  // Miliseconds

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
  } else if (var == "RESTINTERVAL") {
    return String(TicksToMinutes(g_ulFansRestInterval));
  } else if (var == "RESTDUR") {
    return String(TicksToMinutes(g_ulFansRestDuration));
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
    digitalWrite(pRelayModulePins[i].Pin, PIN_OFF);  // Set default Pin State

    snprintf(cLogBuffer, sizeof(cLogBuffer), "%s Pin Done!", pRelayModulePins[i].Name);
    LOGGER(cLogBuffer, INFO);
  }

  ledcAttach(S8050_PWM_PIN, S8050_FREQUENCY, S8050_RESOLUTION);
  LOGGER("Light Brightness Pin Done!", INFO);

  pinMode(HW080_VCC_PIN, OUTPUT);
  digitalWrite(HW080_VCC_PIN, PIN_ON);
  LOGGER("Power Pin for Soil Humidity Sensors Done!", INFO);

  for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++) {
    pinMode(nSoilHumidityPins[i], INPUT);

    snprintf(cLogBuffer, sizeof(cLogBuffer), "Soil Humidity Pin %d Done!", i);
    LOGGER(cLogBuffer, INFO);
  }

  ConnectSD(true);  // Try to init SD Card

  LOGGER("Loading Settings & Time...", INFO);

  if (g_bIsSDInit) {
    File pSettingsFile = SD.open("/settings", FILE_READ); // Read Settings File
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

      g_cSSIDPWD[sizeof(g_cSSIDPWD) - 1] = '\0';
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // SELECTED PROFILE
      g_nCurrentProfile = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // FANS REST INTERVAL
      g_ulFansRestInterval = atoi(cBuffer);
      ///////////////////////////////////////////////////
      cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // FANS REST DURATION
      g_ulFansRestDuration = atoi(cBuffer);
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
      pSettingsFile.close();
      ///////////////////////////////////////////////////
      ProfilesLoader();  // LOAD PROFILES VALUES

      ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - (g_pSettings[g_nCurrentProfile].LightBrightness < 401 ? 0 : g_pSettings[g_nCurrentProfile].LightBrightness));  // WARNING: Hardcode offset
    } else {
      LOGGER("Failed to open Settings file.", ERROR);
    }
    ///////////////////////////////////////////////////
    File pTimeFile = SD.open("/time", FILE_READ); // Read Time file
    if (pTimeFile) {
      LOGGER("Getting Datetime from SD Card...", INFO);

      struct timeval tv;
      String strTimestamp = pTimeFile.readStringUntil('\n');
      tv.tv_sec = strTimestamp.toInt();
      tv.tv_usec = 0;

      settimeofday(&tv, nullptr);

      configTime(TIMEZONE_UTC_OFFSET, TIMEZONE_DST_OFFSET, nullptr, nullptr);

      time_t timeNow = time(nullptr);
      struct tm timeInfo;

      localtime_r(&timeNow, &timeInfo);

      LOGGER("Current Datetime: %04d-%02d-%02d %02d:%02d:%02d.", INFO, timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

      pTimeFile.close();
    } else {
      LOGGER("Failed to open Time file.", ERROR);
    }
    ///////////////////////////////////////////////////
    File pMetricsFile = SD.open("/metrics.log", FILE_READ);  // Read Time file
    if (pMetricsFile) {
      size_t sizeFile = pMetricsFile.size();
      const size_t sizeBlock = 512;
      char cChunkBuffer[sizeBlock];
      int64_t nPos = sizeFile - sizeBlock;
      uint8_t nLinesRead = 0;

      while (nPos >= 0 && nLinesRead < MAX_GRAPH_MARKS) {
        pMetricsFile.seek(nPos);

        size_t sizeBytesRead = pMetricsFile.read((uint8_t*)cChunkBuffer, (nPos < sizeBlock) ? nPos + 1 : sizeBlock);
        if (sizeBytesRead == 0)
          break;

        for (int i = (int)sizeBytesRead - 1; i >= 0; i--) {
          if (cChunkBuffer[i] == '\n' || (nPos == 0 && i == 0)) {
            pMetricsFile.seek(nPos + i + 1);

            char cBuffer[64];
            size_t nBytesRead = pMetricsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1);
            cBuffer[nBytesRead] = '\0';

            if (nBytesRead > 0) {
              while (nBytesRead > 0 && (cBuffer[nBytesRead - 1] == '\r' || cBuffer[nBytesRead - 1] == ' '))
                cBuffer[--nBytesRead] = '\0';

              g_strArrayGraphData[nLinesRead] = strdup(cBuffer);

              nLinesRead++;
            }

            if (nLinesRead >= MAX_GRAPH_MARKS)
              break;
          }
        }

        if (nLinesRead >= MAX_GRAPH_MARKS || nPos == 0)
          break;

        nPos -= sizeBlock;
        if (nPos < 0)
          nPos = 0;
      }

      pMetricsFile.close();
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
      delay(WIFI_CHECK_INTERVAL);  // Wait before trying again
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
    /*IPAddress pClientIP = request->client()->getRemoteAddress();
    LOGGER("HTTP Request IP From: %d.%d.%d.%d.", INFO, pClientIP[0], pClientIP[1], pClientIP[2], pClientIP[3]);*/

    if (request->hasArg("action")) {  // Process the request
      if (request->arg("action") == "reload") {  // This is for reload Elements from the Panel based on selected Profile
        String strReturn = "";

        if (request->hasArg("profilesel")) {
          uint8_t nSelectedProfile = request->arg("profilesel").toInt();

          strReturn += "lightstart:" + String(g_pSettings[nSelectedProfile].StartLightTime);
          strReturn += ":lightstop:" + String(g_pSettings[nSelectedProfile].StopLightTime);
          strReturn += ":lightbright:" + String(g_pSettings[nSelectedProfile].LightBrightness);
          ///////////////////////////////////////////////////
          strReturn += ":intfansta:" + String(g_pSettings[nSelectedProfile].StartInternalFanTemperature);
          ///////////////////////////////////////////////////
          strReturn += ":venttempstart:" + String(g_pSettings[nSelectedProfile].StartVentilationTemperature);
          strReturn += ":venthumstart:" + String(g_pSettings[nSelectedProfile].StartVentilationHumidity);
          ///////////////////////////////////////////////////
          strReturn += ":currentwateringday:" + String(g_pSettings[nSelectedProfile].CurrentWateringDay);

          bool bFirst = true;
          strReturn += ":wateringchart:";

          for (const auto& Watering : g_pSettings[nSelectedProfile].WateringStages) {
            if (!bFirst)
              strReturn += ",";
            else
              bFirst = false;

            strReturn += String(Watering.Day) + "|" + String(Watering.TargetCC);
          }
        }

        if (strReturn != "")
          request->send(200, "text/plain", "RELOAD" + strReturn);
      } else if (request->arg("action") == "testpump") {
        String strReturn = "La bomba no se puede probar en este momento.";

        if (g_ulTestWateringPumpStartTime == 0 && g_ulWateringDuration <= 0) {
          g_bTestPump = true;

          digitalWrite(GetPinByName("Water Pump"), PIN_ON);

          strReturn = "La bomba estará encendida durante los próximos 60 segundos.";

          LOGGER("Watering Pump Flow test started.", INFO);
        }

        if (strReturn != "")
          request->send(200, "text/plain", "MSG" + strReturn);
      } else if (request->arg("action") == "restart") {
        LOGGER("Restarting Controller by Web command.", INFO);
        ESP.restart();
      } else if (request->arg("action") == "update") {  // This is for update Settings
        uint8_t nSelectedProfile = request->arg("profilesel").toInt();
        unsigned long lNewValue;
        String strReturn = "";
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

          LOGGER("New Datetime: %04d-%02d-%02d %02d:%02d:%02d.", INFO, timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

          strReturn += "Se sincronizó la Fecha actual.\r\n";
        }
        // =============== Profile Selector =============== //
        if (request->hasArg("profilesel")) {
          if (nSelectedProfile != g_nCurrentProfile) {
            g_nCurrentProfile = nSelectedProfile;

            ProfilesLoader();

            // Check if currently are watering, and stop it
            if (g_ulWateringDuration > 0 && !digitalRead(GetPinByName("Water Pump"))) {
              digitalWrite(GetPinByName("Water Pump"), PIN_OFF);

              LOGGER("Watering Finished because profile has been changed.", INFO);
            }

            // Reset Watering variables
            g_nLastResetDay = -1;
            g_ulWateringDuration = 0;
            memset(g_bWateredHour, 0, sizeof(g_bWateredHour));

            strReturn += "Se cambió al Perfil de: ";
            strReturn += (nSelectedProfile == 0) ? "Vegetativo" : ((nSelectedProfile == 1) ? "Floración" : "Secado");
            strReturn += ".\r\n";
          }
        }
        // =============== Light Start =============== //
        if (request->hasArg("lightstart")) {
          lNewValue = request->arg("lightstart").toInt();

          if (lNewValue != g_pSettings[nSelectedProfile].StartLightTime) {
            g_pSettings[nSelectedProfile].StartLightTime = lNewValue;
            g_nEffectiveStartLights = (g_pSettings[nSelectedProfile].StartLightTime == 24) ? 0 : g_pSettings[nSelectedProfile].StartLightTime;  // Stores the effective light start hour, converting 24 to 0 (midnight)

            strReturn += "Se actualizó la Hora de Encendido de Luz.\r\n";
          }
        }
        // =============== Light Stop =============== //
        if (request->hasArg("lightstop")) {
          lNewValue = request->arg("lightstop").toInt();

          if (lNewValue != g_pSettings[nSelectedProfile].StopLightTime) {
            g_pSettings[nSelectedProfile].StopLightTime = lNewValue;
            g_nEffectiveStopLights = (g_pSettings[nSelectedProfile].StopLightTime == 0) ? 24 : g_pSettings[nSelectedProfile].StopLightTime; // Stores the effective light stop hour, converting 0 to 24 (midnight)

            strReturn += "Se actualizó la Hora de Apagado de Luz.\r\n";
          }
        }
        // =============== Light Brightness =============== //
        if (request->hasArg("lightbright")) {
          lNewValue = request->arg("lightbright").toInt();

          if (lNewValue != g_pSettings[nSelectedProfile].LightBrightness) {
            g_pSettings[nSelectedProfile].LightBrightness = lNewValue < 401 ? 0 : lNewValue;

            ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - g_pSettings[nSelectedProfile].LightBrightness);

            strReturn += "Se actualizó la Intensidad de Luz.\r\n";
          }
        }
        // =============== Internal Fan Start =============== //
        if (request->hasArg("intfansta")) {
          lNewValue = request->arg("intfansta").toInt();

          if (lNewValue != g_pSettings[nSelectedProfile].StartInternalFanTemperature) {
            g_pSettings[nSelectedProfile].StartInternalFanTemperature = lNewValue;

            strReturn += "Se actualizó la Temperatura de Encendido del Ventilador Interno.\r\n";
          }
        }
        // =============== Ventilation Temperature Start =============== //
        if (request->hasArg("venttempstart")) {
          lNewValue = request->arg("venttempstart").toInt();

          if (lNewValue != g_pSettings[nSelectedProfile].StartVentilationTemperature) {
            g_pSettings[nSelectedProfile].StartVentilationTemperature = lNewValue;

            strReturn += "Se actualizó la Temperatura de Encendido de Recirculación.\r\n";
          }
        }
        // =============== Ventilation Humidity Start =============== //
        if (request->hasArg("venthumstart")) {
          lNewValue = request->arg("venthumstart").toInt();

          if (lNewValue != g_pSettings[nSelectedProfile].StartVentilationHumidity) {
            g_pSettings[nSelectedProfile].StartVentilationHumidity = lNewValue;

            strReturn += "Se actualizó la Humedad de Encendido de la Recirculación.\r\n";
          }
        }
        // =============== Current Watering Day =============== //
        if (request->hasArg("currentwateringday")) {
          lNewValue = request->arg("currentwateringday").toInt();

          if (lNewValue != g_pSettings[nSelectedProfile].CurrentWateringDay) {
            g_pSettings[nSelectedProfile].CurrentWateringDay = lNewValue;

            strReturn += "Se actualizó los Días de Riego transcurridos.\r\n";
          }
        }
        // =============== Watering Chart =============== //
        if (request->hasArg("wateringchart")) {
          String strValues = request->arg("wateringchart");
          uint16_t nIndex = 0;
          std::vector<WateringData> vecNewWateringStages;

          while (nIndex < strValues.length()) {
            int nCommaIndex = strValues.indexOf(',', nIndex);
            String strPair = (nCommaIndex == -1) ? strValues.substring(nIndex) : strValues.substring(nIndex, nCommaIndex);

            int nSepIndex = strPair.indexOf('|');
            if (nSepIndex != -1)
              vecNewWateringStages.push_back({ strPair.substring(0, nSepIndex).toInt(), strPair.substring(nSepIndex + 1).toInt() });

            if (nCommaIndex == -1)
              break;

            nIndex = nCommaIndex + 1;
          }

          if (vecNewWateringStages != g_pSettings[nSelectedProfile].WateringStages) {
            g_pSettings[nSelectedProfile].WateringStages = vecNewWateringStages;

            strReturn += "Se actualizó el Esquema de Riesgo.\r\n";
          }
        }
        // =============== Fans Rest Interval =============== //
        if (request->hasArg("restinterval")) {
          lNewValue = MinutesToTicks(request->arg("restinterval").toInt());

          if (lNewValue != g_ulFansRestInterval) {
            g_ulFansRestInterval = lNewValue;

            strReturn += "Se actualizó el Intervalo de Reposo de Ventiladores.\r\n";
          }
        }
        // =============== Fans Rest Duration =============== //
        if (request->hasArg("restdur")) {
          lNewValue = MinutesToTicks(request->arg("restdur").toInt());

          if (lNewValue != g_ulFansRestDuration) {
            g_ulFansRestDuration = lNewValue;

            strReturn += "Se actualizó la Duración de Reposo de Ventiladores.\r\n";
          }
        }
        // =============== Environment Temperature Hysteresis =============== //
        if (request->hasArg("temphys")) {
          lNewValue = request->arg("temphys").toInt();

          if (lNewValue != g_nTemperatureStopHysteresis) {
            g_nTemperatureStopHysteresis = lNewValue;

            strReturn += "Se actualizó la Histéresis de Apagado por Temperatura.\r\n";
          }
        }
        // =============== Environment Humidity Hysteresis =============== //
        if (request->hasArg("humhys")) {
          lNewValue = request->arg("humhys").toInt();

          if (lNewValue != g_nHumidityStopHysteresis) {
            g_nHumidityStopHysteresis = lNewValue;

            strReturn += "Se actualizó la Histéresis de Apagado por Humedad.\r\n";
          }
        }
        // =============== Drip Per Minute =============== //
        if (request->hasArg("dripperminute")) {
          lNewValue = request->arg("dripperminute").toInt();

          if (lNewValue != g_nDripPerMinute) {
            g_nDripPerMinute = lNewValue;

            strReturn += "Se actualizó el valor de Tasa de Goteo por Minuto.\r\n";
          }
        }
        // =============== WiFi =============== //
        bool bWiFiChanges = false;

        if (request->hasArg("ssid") && strcmp(request->arg("ssid").c_str(), g_cSSID) != 0) {
          bWiFiChanges = true;

          strReturn += "Se actualizó el SSID de Wifi.\r\n";
        }

        if (request->hasArg("ssidpwd") && strcmp(request->arg("ssidpwd").c_str(), g_cSSIDPWD) != 0) {
          bWiFiChanges = true;

          strReturn += "Se actualizó la Contraseña de Wifi.\r\n";
        }

        if (bWiFiChanges) {
#ifdef ENABLE_AP_ALWAYS
          strReturn += "Se intentará conectar a la nueva Red";
#else
          strReturn += "Se intentará conectar a la nueva Red, de no ser posible; se iniciará una Red Wifi (" + String(ACCESSPOINT_NAME) + ") para poder reconfigurar el controlador";
#endif
          strReturn += ".\r\n";
        }
        //////////////////////////////////////////////////
        if (strReturn != "") {  // If have some change, send response to web client and finally save new settings values
          request->send(200, "text/plain", "MSG" + strReturn);

          if (bWiFiChanges) { // Update Wifi values after response the request. in otherwise the message is not sended.
            strncpy(g_cSSID, request->arg("ssid").c_str(), sizeof(g_cSSID) - 1);
            g_cSSID[sizeof(g_cSSID) - 1] = '\0';

            strncpy(g_cSSIDPWD, request->arg("ssidpwd").c_str(), sizeof(g_cSSIDPWD) - 1);
            g_cSSIDPWD[sizeof(g_cSSIDPWD) - 1] = '\0';
          }

          ConnectSD(false);

          if (g_bIsSDInit) {
            File pSettingsFile = SD.open("/settings", FILE_WRITE);  // Save Internal Settings
            if (pSettingsFile) {
              pSettingsFile.println(g_cSSID);
              pSettingsFile.println(g_cSSIDPWD);

              pSettingsFile.println(nSelectedProfile);

              pSettingsFile.println(g_ulFansRestInterval);
              pSettingsFile.println(g_ulFansRestDuration);

              pSettingsFile.println(g_nTemperatureStopHysteresis);
              pSettingsFile.println(g_nHumidityStopHysteresis);

              pSettingsFile.println(g_nDripPerMinute);

              LOGGER("Settings file updated successfully.", INFO);

              pSettingsFile.close();
            }
            ///////////////////////////////////////////////////
            WriteProfile(nSelectedProfile);
          } else {
            LOGGER("Cannot connect to SD to save new settings!", ERROR);
          }
        }

        if (bWiFiChanges) { // After send response to web client, Try reconnect to Wifi if is required
          LOGGER("Disconnecting Wifi to start connection to new SSID...", WARN);

          WiFi.disconnect(false); // First disconnect from current Network (Arg false to just disconnect the Station, not the AP)

          if (eTaskGetState(pWiFiReconnect) == eRunning)  // Then check if task pWiFiReconnect is running. If it is, Suspend it to eventually Resume it with the new SSID and SSID Password
            vTaskSuspend(pWiFiReconnect);
        }
      } else if (request->arg("action") == "refresh") { // This is for refresh Panel values
        uint8_t nSelectedProfile = request->arg("profilesel").toInt();
        String strResponse = "";
        // ================================================== Environment Section ================================================== //
        strResponse += String(g_nEnvironmentTemperature) + ":" + String(g_nEnvironmentHumidity) + ":" + String(g_fEnvironmentVPD, 2);
        // ================================================== Soil Section ================================================== //
        strResponse += ":";
        bool bFirst = true;

        for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++) {
          if (!bFirst)
              strResponse += ",";
            else
              bFirst = false;

          strResponse += String(g_nSoilsHumidity[i]);
        }
        // ================================================== Current Time Section ================================================== //
        time_t timeNow = time(nullptr);

        strResponse += ":" + String(timeNow);
        // ================================================== Light Brightness slider position Section ================================================== //
        strResponse += ":" + String(g_pSettings[nSelectedProfile].LightBrightness < 401 ? 0 : g_pSettings[nSelectedProfile].LightBrightness);
        // ================================================== Fans Rest State Section ================================================== //
        strResponse += ":";

        if (g_bFansRest)
          strResponse += String(g_ulFansRestDuration - (millis() - g_ulFansRestElapsedTime)); // Miliseconds  // TODO: Esto lo tendría que enviar en segundos. Aparte, me parece que sería mejor tener la variable ulCurrentMillis declarada globalmente y obtener el tick actual directamente de ahí. aparte, tengo que remover la división entre 1000 en el código de JS
        else
          strResponse += "0";

        // Internal Fan
        strResponse += ":" + String(digitalRead(GetPinByName("Internal Fan"))); // 1 = stopped, 0 turn on

        // Ventilation
        strResponse += ":" + String(digitalRead(GetPinByName("Ventilation")));  // 1 = stopped, 0 turn on
        // ================================================== Watering Section ================================================== //
        strResponse += ":" + String(g_pSettings[nSelectedProfile].CurrentWateringDay);
        strResponse += ":" + String(g_ulWateringDuration);  // Seconds
        // ================================================== Graph Section ================================================== //
        size_t sizeGraphArraySize = sizeof(g_strArrayGraphData) / sizeof(g_strArrayGraphData[0]);

        if (sizeGraphArraySize > 0 && g_strArrayGraphData[0] != nullptr) {
          strResponse += ":";

          for (size_t i = sizeGraphArraySize - 1; i >= 0; i--) {
            if (g_strArrayGraphData[i] != nullptr) {
              if (i < (sizeGraphArraySize - 1))
                strResponse += ",";

              strResponse += String(g_strArrayGraphData[i]);
            }
          }
        }
        // ========================================================================================================================= //
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain;charset=utf-8", "REFRESH" + strResponse);
        /*
          Response structure example: each data[X] is divided by:
          data[0] → Environment Temperature
          data[1] → Environment Humidity
          data[2] → Environment VPD
          data[3] → <Soil Moistures values Array> Example: SOIL 1 MOISTURE VALUE,SOIL 2 MOISTURE VALUE
          data[4] → Current Timestamp
          data[5] → Light Brightness
          data[6] → Fans Rest time Remaining
          data[7] → Internal Fan State
          data[8] → Ventilation Fan State
          data[9] → Current Watering Day
          data[10] → Watering Time Remaining
          data[11] → <History Chart values Array> Example: Unix Timestamp|Environment Temperature|Environment Humidity|VPD|<Soil Moistures values Array>
        */
        request->send(response);
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
  static unsigned long ulLastSecondTick = 0;  // General
  static unsigned long ulLastStoreElapsedTime = 0;  // To Save Environment values to show in Graph
  unsigned long ulCurrentMillis = millis(); 

  if ((ulCurrentMillis - ulLastSecondTick) >= 1000) { // Check if 1 second has passed since the last tick to perform once-per-second tasks
    ulLastSecondTick = ulCurrentMillis;
    time_t timeNow = time(nullptr);
    struct tm *timeInfo = localtime(&timeNow);
    // ================================================== Wifi Section ================================================== //
    if (ulCurrentMillis >= (g_ulStartUpTime + (WIFI_CHECK_INTERVAL * WIFI_MAX_RETRYS)) && eTaskGetState(pWiFiReconnect) == eSuspended && WiFi.status() != WL_CONNECTED) // If is not connected to Wifi and is not currently running a reconnect trask, start it
      vTaskResume(pWiFiReconnect);
    // ================================================== Time Section ================================================== //
    WriteToSD("/time", String((uint32_t)timeNow), false); // Write current time to SD Card
    // ================================================== Environment Section ================================================== //
    GetEnvironmentParameters(); // Update Environment parameters in Global variables
    // ================================================== Lights & Watering Sections ================================================== //
    if ((g_pSettings[g_nCurrentProfile].StartLightTime > 0 || g_pSettings[g_nCurrentProfile].StopLightTime > 0) &&
          (g_nEffectiveStartLights < g_nEffectiveStopLights && timeInfo->tm_hour >= g_nEffectiveStartLights && timeInfo->tm_hour < g_nEffectiveStopLights) || // If the Start hour is earlier than the Stop hour (e.g., from 8 to 20), turn on the lights only if the current hour is within that range
          (g_nEffectiveStartLights >= g_nEffectiveStopLights && (timeInfo->tm_hour >= g_nEffectiveStartLights || timeInfo->tm_hour < g_nEffectiveStopLights))) {  // If the Start hour is later than or equal to the Stop hour (e.g., from 20 to 6), turn on the Lights if the current hour is either after the Start or before the Stop
      if (digitalRead(GetPinByName("Lights"))) {
        digitalWrite(GetPinByName("Lights"), PIN_ON);

        LOGGER("Lights Started.", INFO);
      }
      ///////////////////////////////////////////////////
      if (timeInfo->tm_hour == 0 && timeInfo->tm_mday != g_nLastResetDay) {
        g_nLastResetDay = timeInfo->tm_mday;

        for (uint8_t i = 0; i < 24; ++i)
          g_bWateredHour[i] = false;

        g_pSettings[g_nCurrentProfile].CurrentWateringDay++;
        WriteProfile(g_nCurrentProfile);
      }

      if (g_ulWateringDuration > 0) {
        g_ulWateringDuration--; // Decrease -1 by each second pass

        if (g_ulWateringDuration <= 0 && !digitalRead(GetPinByName("Water Pump"))) {
          digitalWrite(GetPinByName("Water Pump"), PIN_OFF);

          g_ulWateringDuration = 0;

          LOGGER("Watering Finished.", INFO);
        }
      }

      if (!g_bWateredHour[timeInfo->tm_hour]) {
        uint16_t nLastKnownCC = 0;
        uint8_t nStartWaterHour = (g_nEffectiveStartLights + 2) % 24;
        uint8_t nStopWaterHour = (g_nEffectiveStopLights - 2 + 24) % 24;
        int8_t nTotalPulses = (nStopWaterHour - nStartWaterHour + 24) % 24;

        for (const auto& Watering : g_pSettings[g_nCurrentProfile].WateringStages) {
          if (g_pSettings[g_nCurrentProfile].CurrentWateringDay >= Watering.Day)
            nLastKnownCC = Watering.TargetCC;
          else
            break;
        }

        float fCCPerPulse = (nTotalPulses > 0) ? ((float)nLastKnownCC / nTotalPulses) : 0.0f;
        float fPulseTime = (g_nDripPerMinute > 0) ? (fCCPerPulse / g_nDripPerMinute) * 60.0f : 0.0f;  // Seconds
        String strIrrigationHours = "";

        for (uint8_t i = 0; i < nTotalPulses; i++) {
          uint8_t nHour = (nStartWaterHour + i) % 24;

          if (nHour <= timeInfo->tm_hour)// NOTE: Here I have a dilemma. Can I mark the previous hours as watered, or even the previous hours and the current hour. If I mark only the previous hours and not the current one, it could happen that the current hour's watering is completed, then the power goes out immediately, and when the controller is restarted, it starts watering again. This would accumulate two waterings in close proximity, potentially producing an excess of irrigation solution.
            g_bWateredHour[nHour] = true;

          if (!g_bWateredHour[timeInfo->tm_hour] && nHour == timeInfo->tm_hour) {
            g_ulWateringDuration = ceil(fPulseTime);  // Round up and cast to unsigned long
            g_bWateredHour[nHour] = true;

            if (digitalRead(GetPinByName("Water Pump"))) {
              digitalWrite(GetPinByName("Water Pump"), PIN_ON);

              LOGGER("Watering Started.", INFO);
            }
          }

          uint8_t nHourLabel = nHour % 12;

          if (nHourLabel == 0)
            nHourLabel = 12;

          strIrrigationHours += " " + String(nHourLabel) + (nHour >= 12 ? "PM" : "AM");
        }

        g_bWateredHour[timeInfo->tm_hour] = true;

        LOGGER("Total Irrigation Pulses: %d | CC Per Pulse: %.1f | Pulse Duration: %.1f seconds | Pulse Hours:%s", INFO, nTotalPulses, fCCPerPulse, fPulseTime, strIrrigationHours.c_str());
      }
    } else {  // If Current Time is out of ON range, turn it OFF
      if (!digitalRead(GetPinByName("Lights"))) {
        digitalWrite(GetPinByName("Lights"), PIN_OFF);

        LOGGER("Lights Stopped.", INFO);
      }
    }
    // ================================================== Fans Section ================================================== //
    if (!g_bFansRest && g_ulFansRestInterval > 0) { // If is not Rest time for Fans & check if g_ulFansRestInterval just in case of SD is not connected at Controller startup
      static unsigned long ulFansRestIntervalTime = 0;

      if ((ulCurrentMillis - ulFansRestIntervalTime) >= g_ulFansRestInterval) {  // First check if need go in Rest time
        ulFansRestIntervalTime = ulCurrentMillis;
        g_ulFansRestElapsedTime = ulCurrentMillis;
        // Force Fans stop
        digitalWrite(GetPinByName("Internal Fan"), PIN_OFF);
        digitalWrite(GetPinByName("Ventilation"), PIN_OFF);

        g_bFansRest = true;

        LOGGER("Fans Rest mode started.", INFO);
      } else {  // Is not
        if (g_nEnvironmentTemperature > 0 && g_nEnvironmentHumidity > 0) {  // Check if Environment Sensor is working
          // ================================================== Internal Fan control by Temperature ================================================== //
          if (g_nEnvironmentTemperature >= g_pSettings[g_nCurrentProfile].StartInternalFanTemperature) {  // If Environment Temperature if higher or equal to needed to Start Internal Fan Temperature, start it
            if (digitalRead(GetPinByName("Internal Fan"))) {
              digitalWrite(GetPinByName("Internal Fan"), PIN_ON);

              LOGGER("Internal Fan Started.", INFO);
            }
          } else if (g_nEnvironmentTemperature <= (g_pSettings[g_nCurrentProfile].StartInternalFanTemperature - g_nTemperatureStopHysteresis)) {  // If Environment Temperature is less or equal to: Start Internal Fan Temperature - Temperature Hysteresis, stop it
            if (!digitalRead(GetPinByName("Internal Fan"))) {
              digitalWrite(GetPinByName("Internal Fan"), PIN_OFF);

              LOGGER("Internal Fan Stopped.", INFO);
            }
          }
          // ================================================== Ventilation Fans control by Temperature & Humidity ================================================== //
          static bool bStartVentilationByTemperature = false;
          static bool bStartVentilationByHumidity = false;

          // Control by Temperature
          if (g_nEnvironmentTemperature >= g_pSettings[g_nCurrentProfile].StartVentilationTemperature)
            bStartVentilationByTemperature = true;
          else if (g_nEnvironmentTemperature <=(g_pSettings[g_nCurrentProfile].StartVentilationTemperature - g_nTemperatureStopHysteresis))
            bStartVentilationByTemperature = false;

          // Control by Humidity
          if (g_nEnvironmentHumidity >= g_pSettings[g_nCurrentProfile].StartVentilationHumidity)
            bStartVentilationByHumidity = true;
          else if (g_nEnvironmentHumidity <= (g_pSettings[g_nCurrentProfile].StartVentilationHumidity - g_nHumidityStopHysteresis))
            bStartVentilationByHumidity = false;
          ///////////////////////////////////////////////////
          if (bStartVentilationByTemperature || bStartVentilationByHumidity) {
            if (digitalRead(GetPinByName("Ventilation"))) {
                digitalWrite(GetPinByName("Ventilation"), PIN_ON);

                LOGGER("Ventilation Started.", INFO);
              }
          } else {
            if (!digitalRead(GetPinByName("Ventilation"))) {
              digitalWrite(GetPinByName("Ventilation"), PIN_OFF);

              LOGGER("Ventilation Stopped.", INFO);
            }
          }
        }
      }
    } else {  // If Fans are in Rest time
      if (g_ulFansRestDuration > 0 && (ulCurrentMillis - g_ulFansRestElapsedTime) >= g_ulFansRestDuration) {  // Check if g_ulFansRestDuration just in case of SD is not connected at Controller startup
        g_bFansRest = false;
        g_ulFansRestElapsedTime = 0;

        LOGGER("Fans Rest time completed.", INFO);
      }
    }
    // ================================================== Store Data for Graph Section ================================================== //
    if ((ulCurrentMillis - ulLastStoreElapsedTime) >= GRAPH_MARKS_INTERVAL) {
      ulLastStoreElapsedTime = ulCurrentMillis;

      String strValues = String(timeNow) + "|" + String(g_nEnvironmentTemperature) + "|" + String(g_nEnvironmentHumidity) + "|" + String(g_fEnvironmentVPD, 2);

      for (uint8_t i = 0; i < nSoilHumidityPinsCount; i++) {
        g_nSoilsHumidity[i] = GetSoilHumidity(i);

        strValues += "|" + String(g_nSoilsHumidity[i]);
      }

      WriteToSD("/metrics.log", strValues, true);

      for (uint8_t i = MAX_GRAPH_MARKS - 1; i > 0; i--) { // Shifts all values ​​up one position
        if (g_strArrayGraphData[i]) {
          free(g_strArrayGraphData[i]);
          g_strArrayGraphData[i] = nullptr; // Just in case...
        }
  
        g_strArrayGraphData[i] = g_strArrayGraphData[i - 1] ? strdup(g_strArrayGraphData[i - 1]) : nullptr; // If the previous position has a value, pass it to the current iteration position. Otherwise, set it to null.
      }

      if (g_strArrayGraphData[0]) {
        free(g_strArrayGraphData[0]);
        g_strArrayGraphData[0] = nullptr; // Just in case...
      }

      g_strArrayGraphData[0] = strdup(strValues.c_str()); // Store the current value at the beginning of the array
    }
    // ================================================== Watering Pump Flow Test Section ================================================== //
    if (g_bTestPump) {
      g_ulTestWateringPumpStartTime = ulCurrentMillis;
      g_bTestPump = false;
    }

    if (g_ulTestWateringPumpStartTime > 0 && (ulCurrentMillis - g_ulTestWateringPumpStartTime) >= 60000) { // Check if has been pass 60 seconds. If is, turn off the pump
      if (!digitalRead(GetPinByName("Water Pump"))) {
        digitalWrite(GetPinByName("Water Pump"), PIN_OFF);

        LOGGER("Watering Pump Flow test Finished.", INFO);
      }

      g_ulTestWateringPumpStartTime = 0;
    }
  }
}
