//          _________________________________________________________________
//         /                                                                /\
//        /  _   __    _                   __                   ______     / /\
//       /  / | / /   (_)  _____  ____    / /  ____ _   _____  / ____/  __/ /
//      /  /  |/ /   / /  / ___/ / __ \  / /  / __ `/  / ___/ / / __   /\_\/
//     /  / /|  /   / /  / /__  / /_/ / / /  / /_/ /  (__  ) / /_/ /  /_/
//    /  /_/ |_/   /_/   \___/  \____/ /_/   \__,_/  /____/  \____/    /\
//   /                        Version 4 (2025)                        / /
//  /________________________________________________________________/ /
//  \________________________________________________________________\/
//   \    \    \    \    \    \    \    \    \    \    \    \    \    \

#include <SD.h> // https://docs.arduino.cc/libraries/sd/#SD%20class
#include <WiFi.h>
#include <time.h>
#include <vector>
#include <Update.h>
#include <ESPmDNS.h>
#include <SimpleDHT.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
// TODO: A futuro sería ideal agregar un archivo durante el proceso de incorporación de Fertilizantes. Así en caso de pérdida de energía, se pueda reanudar el proceso donde se haya quedado.
// TODO: A futuro podría reescribir toda la lógica para poder funcionar con días de mas de 24 horas (trabajar con marcas de tiempo transcurrido en lugar de horas y días).
struct RelayPin {
  const char* Name; // Channel name
  uint8_t Pin;      // Pin number
};

struct SoilMoisturePin {
  uint8_t Pin;  // Pin Number
  const char HTMLColor[8];  // HTML Color for use in Graph on Web Panel
};

struct WateringData {
  uint8_t Day;  // WARNING: Maybe is too low, only 8 months
  uint16_t TargetCC;

  bool operator==(const WateringData& other) const { return Day == other.Day && TargetCC == other.TargetCC; } // Little overload but needed for compare
};

struct ProfileSettings {
  uint8_t StartLightTime;
  uint8_t StopLightTime;
  uint16_t LightBrightness;

  uint8_t StartInternalFanTemperature;

  uint8_t StartVentilationTemperature;
  uint8_t StartVentilationHumidity;

  uint16_t PHReducerToApply;
  uint16_t VegetativeFertilizerToApply;
  uint16_t FloweringFertilizerToApply;

  uint16_t IrrigationDayCounter;
  uint8_t LastWateredDay;
  int8_t LastWateredHour;

  std::vector<WateringData> WateringStages;
};

// NOTES:
// Default IP for AP mode is: 192.168.4.1
// If Environment Humidity or Temperature Reads 0, the fans never gonna start.
// If Light Start & Stop Times Is 0, the light never gonna start.
// DHT11 have a pullup (between data and vcc)
// HW080 have a pulldown (in return line to gnd)
// All logics assume/is for Days of 24 Hours max

// Definitions
//#define ENABLE_SERIAL_LOGGER  // Use this when debugging
//#define ENABLE_SD_LOGGING     // Use this to save logs to SD Card
//#define ENABLE_AP_ALWAYS      // Use this to enable always the Access Point. Else it just enable when have no internet connection

#define MAX_PROFILES 3  // 0 Vegetative (Filename: veg), 1 Flowering (Filename: flo), 2 Drying (Filename: dry)

#define MAX_GRAPH_MARKS 48  // How much logs show in Web Panel Graph
#define MAX_GRAPH_MARKS_LENGTH 36 // How long text is (Example: 1749390362|100|99|0.02|100|100|4095) If change from DHT11 to DHT22 can be 1 more byte more. For each extra soil moisture sensor is 4 bytes more. Remember add a extra byte for null terminator

#define WIFI_MAX_RETRYS 5 // Max Wifi reconnection attempts
#define WIFI_CHECK_INTERVAL 1000  /*1 Second*/

#define WEBSERVER_PORT 80

#define DNS_ADDRESS "indoor"  // http://"DNS_ADDRESS".local/
#define ACCESSPOINT_NAME "ESP32_Indoor"

#define TIMEZONE "America/Argentina/Buenos_Aires"

#define CALLMEBOT_APY_KEY TODO:...
#define CALLMEBOT_PHONE_TO_SEND "TODO:..."

#define S8050_FREQUENCY 300   // https://www.mouser.com/datasheet/2/149/SS8050-117753.pdf
#define S8050_RESOLUTION 12
#define S8050_MAX_VALUE 4095  // Cuz is 12 bits of resolution

#define HW080_MIN 0         // https://cms.katranji.com/web/content/723081
#define HW080_MAX 2800      // I'm using 20k resistors, so the max value never go up to 4095
#define HW080_MAX_READS 10  // To get good soil humidity average

#define DHT_MAX_READS 5 // To get average

#define HCSR04_MAX_READS 5  // To get average

// Pins
#define DHT_DATA_PIN 4 // https://www.mouser.com/datasheet/2/758/DHT11-Technical-Data-Sheet-Translated-Version-1143054.pdf

#define SD_CS_PIN 5 // Chip select for Enable/Disable SD Card

#define S8050_PWM_PIN 32  // I'm using a 1K resistor in serie in BASE Pin (Light Brightness controller) // https://www.mouser.com/datasheet/2/149/SS8050-117753.pdf

#define HW080_VCC_PIN 13  // I Enable this pin when want to read and disable it to prevent electrolysis

#define HCSR04_TRIGGER_PIN 14
#define HCSR04_ECHO_PIN 33

const RelayPin pRelayModulePins[] = { // Add here more Pins from the Relays Module
  { "Lights", 16 },               // Channel 0 of Relay Module 0  // NOTE: In theory if the driver is turned off with a high value (Because the s8050 transistor is NPN) in S8050_PWM_PIN, I don't need to turn off the lights through the relay and use this channel for something else...
  { "Internal Fan", 17 },         // Channel 1 of Relay Module 0
  { "Ventilation", 18 },          // Channel 2 of Relay Module 0
  { "Mixing Pump", 19 },          // Channel 3 of Relay Module 0
  { "Irrigation Pump", 21 },      // Channel 4 of Relay Module 0
  { "pH Reducer Pump", 22 },      // Channel 5 of Relay Module 0
  { "Vegetative Fert Pump", 23 }, // Channel 6 of Relay Module 0
  { "Flowering Fert Pump", 25 },  // Channel 7 of Relay Module 0
  
  { "Power Supply", 26 }  // Channel 0 of Relay Module 1
  //{ "Water Electrovalve", 27 }, // Channel 1 of Relay Module 1
  //{ "NONE", 13 }, // Channel 2 of Relay Module 1
  //{ "NONE", CAN BE 15 BUT IS "RISKY" }  // Channel 3 of Relay Module 1
};

const SoilMoisturePin pSoilMoisturePins[] = { // Add here more Pins for Soil Moisture
  { 34, "#B57165" },  // Soil Humidity Sensor 0
  { 35, "#784B43" }   // Soil Humidity Sensor 1
  //{ 36, "..." },
  //{ 39, "..." }
};

// Global Constants
const uint8_t nRelayModulePinsCount = sizeof(pRelayModulePins) / sizeof(pRelayModulePins[0]);
const uint8_t nSoilMoisturePinsCount = sizeof(pSoilMoisturePins) / sizeof(pSoilMoisturePins[0]);

// Global Variables
const char* g_strWebServerFiles[] = { // Add here files to be server by the webserver
  "fan.webp",
  "style.css",
  "chart.js"
};

// DO NOT TOUCH IT!
enum ERR_TYPE { INFO, WARN, ERROR };

#define RELAY_PIN_ON  LOW
#define RELAY_PIN_OFF HIGH

// Settings storage Variables
char g_cSSID[32];
char g_cSSIDPWD[32];

uint32_t g_nSamplingInterval = 0;

uint32_t g_nFansRestInterval = 0;
uint32_t g_nFansRestDuration = 0;

uint8_t g_nTemperatureStopHysteresis = 0;
uint8_t g_nHumidityStopHysteresis = 0;

uint16_t g_nIrrigationFlowPerMinute = 0;
uint16_t g_nPHReducerFlowPerMinute = 0;
uint16_t g_nVegetativeFlowPerMinute = 0;
uint16_t g_nFloweringFlowPerMinute = 0;

uint32_t g_nMixingPumpDuration = 0;

uint16_t g_nIrrigationReservoirLowerLevel = 0;

ProfileSettings g_pProfileSettings[MAX_PROFILES] = {};
///////////////////////////////////////////
bool g_bIsSDInit = false;
uint8_t g_nCurrentProfile = 0;
uint8_t g_nEffectiveStartLights = 0;
uint8_t g_nEffectiveStopLights = 0;

uint32_t g_nIrrigationDuration = 0;

uint8_t g_nEnvironmentTemperature = 0;
uint8_t g_nEnvironmentHumidity = 0;
float g_fEnvironmentVPD = 0.0f;
uint8_t g_nIrrigationSolutionLevel = 0;
uint8_t g_nSoilsHumidity[nSoilMoisturePinsCount] = {};

bool g_bFansRest = false;
uint32_t g_nFansRestElapsedTime = 0;

bool g_bTestIrrigationPump = false, g_bTestPHReducerPump = false, g_bTestVegetativeFertPump = false, g_bTestFloweringFertPump = false;
uint32_t g_nTestPumpStartTime = 0;

bool g_bApplyFertilizers = false;

char g_strArrayGraphData[MAX_GRAPH_MARKS][MAX_GRAPH_MARKS_LENGTH] = {};

// Global Handles, Interface & Instances
AsyncWebServer g_pWebServer(WEBSERVER_PORT);  // Asynchronous web server instance listening on WEBSERVER_PORT
SimpleDHT11 g_pDHT11(DHT_DATA_PIN);           // Interface to DHT11 Temperature & Humidity sensor
TaskHandle_t g_pWiFiReconnect;                // Task handle for Wifi reconnect logic running on core 0
SemaphoreHandle_t g_SDMutex;                  // Mutex to synchronize concurrent access to the SD card across tasks

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sets the system time and timezone based on a given Unix timestamp.
// Updates the system's internal clock to the provided timestamp (seconds since epoch).
// Applies the timezone defined by the TIMEZONE macro and refreshes the timezone settings.
void SetCurrentDatetime(time_t unixTimestamp) {
  setenv("TZ", TIMEZONE, 1);
  tzset();

  struct timeval tv;
  tv.tv_sec = unixTimestamp;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Retrieves the current local time as a tm struct.
// Converts the current system time (UTC) to local time according to system timezone settings.
// Returns a struct tm containing broken-down time elements (year, month, day, hour, etc).
struct tm GetLocalTimeNow() {
  time_t timeNow = time(nullptr);
  struct tm timeInfo;
  localtime_r(&timeNow, &timeInfo);

  return timeInfo;
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
uint32_t TicksToSeconds(uint32_t nTicks) { return nTicks / 1000; }
uint32_t TicksToMinutes(uint32_t nTicks) { return nTicks / (1000 * 60); }
//uint32_t TicksToHours(uint32_t nTicks) { return nTicks / (1000 * 60 * 60); }

uint32_t SecondsToTicks(uint32_t nSeconds) { return nSeconds * 1000; }
uint32_t MinutesToTicks(uint32_t nMinutes) { return nMinutes * 1000 * 60; }
//uint32_t HoursToTicks(uint32_t nHours) { return nHours * 1000 * 60 * 60; }

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Returns the name of the first pump that is marked for testing based on the current boolean test flags.
// Only one pump can be tested at a time; the function returns the first active one in priority order.
const char* GetPumpToTest() { // NOTE: A bitmask-based version of this function was benchmarked and showed slightly better performance.
  if (g_bTestIrrigationPump)  //       Consider switching to bitmask logic in the future for improved efficiency and simplified state handling.
    return "Irrigation Pump";
  else if (g_bTestPHReducerPump)
    return "pH Reducer Pump";
  else if (g_bTestVegetativeFertPump)
    return "Vegetative Fert Pump";
  else if (g_bTestFloweringFertPump)
    return "Flowering Fert Pump";

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Logs a formatted message with a severity prefix (INFO, WARNING, or ERROR) to the SD card and/or Serial output.
// The message is built using a printf-style format string and optional arguments.
// Logging targets are controlled via the ENABLE_SD_LOGGING and ENABLE_SERIAL_LOGGER compile-time flags.
// - nType: Logging severity (INFO, WARN, ERROR).
// - format: C-style format string followed by optional values (like printf).
// Prepends the message with a prefix indicating its type and sends it to the enabled outputs.
void LOGGER(ERR_TYPE nType, const char* format, ...) {
#if defined(ENABLE_SD_LOGGING) || defined(ENABLE_SERIAL_LOGGER)
  va_list args;

  char cPrefix[11], cBuffer[512];

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
  WriteToSD("/logging_", cBuffer, true);
#endif
#ifdef ENABLE_SERIAL_LOGGER
  Serial.println(cBuffer);
#endif
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Controls the relay channel associated with the Power Supply by setting its state.
// If bState is true, attempts to turn the relay ON (only if it was previously OFF).
// If bState is false, turns the relay OFF unconditionally.
// Returns true if the relay pin was found and written to; false otherwise.
bool PowerSupplyControl(bool bState) {
  uint8_t nPin = GetPinByName("Power Supply");
  int8_t nState = -1;

  if (bState) {
    if (digitalRead(nPin))
      nState = RELAY_PIN_ON;
    else
      return true;
  } else {
    if (!digitalRead(nPin))
      nState = RELAY_PIN_OFF;
    else
      return true;
  }

  if (nState != -1) {
    digitalWrite(nPin, nState);

    LOGGER(INFO, "The Power Supply was turned %s.", bState ? "ON" : "OFF");

    return true;
  }

  LOGGER(ERROR, "Was not possible to turn %s The Power Supply.", bState ? "ON" : "OFF");

  return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Safely execute the provided function with exclusive access to the SD card.
// Tries to acquire the SD card mutex within 100 ms; if not available, logs a warning and returns.
// If the SD card is not initialized, attempts to initialize it, verifying card presence and filesystem integrity.
// On failure, logs an error, releases the mutex, and returns.
// If initialization succeeds and bShowSuccessLog is true, logs a success message.
// After initialization (or if already initialized), executes the passed function callback.
// Finally, releases the SD card mutex.
void SafeSDAccess(std::function<void()> fn, bool bShowSuccessLog = false) {
  if (!xSemaphoreTake(g_SDMutex, pdMS_TO_TICKS(100  /*ms*/))) {
    LOGGER(WARN, "Could not acquire mutex.");

    return;
  }

  if (!g_bIsSDInit) {
    g_bIsSDInit = SD.begin(SD_CS_PIN);

    if (!g_bIsSDInit) {
      LOGGER(ERROR, "Failed to Initialize SD.");

      xSemaphoreGive(g_SDMutex);

      return;
    }

    if (SD.cardType() == CARD_NONE) {
      LOGGER(ERROR, "No SD Card detected.");

      g_bIsSDInit = false;

      SD.end();

      xSemaphoreGive(g_SDMutex);

      return;
    }

    File pRoot = SD.open("/");
    if (!pRoot || !pRoot.isDirectory()) {
      LOGGER(ERROR, "Filesystem not accessible or corrupted.");

      g_bIsSDInit = false;

      SD.end();

      xSemaphoreGive(g_SDMutex);

      return;
    }

    if (bShowSuccessLog)
      LOGGER(INFO, "SD Initialized.");
  }

  fn();

  xSemaphoreGive(g_SDMutex);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Writes a text line to a file on the SD card safely using SafeSDAccess for thread-safe access.
// If cFileName is "logging_", the actual filename is generated dynamically based on the current date as "logging_YYYY_MM_DD.log".
// Otherwise, uses the provided cFileName directly (truncated if longer than 63 chars).
// Opens the file in append mode if bAppend is true, or overwrites it otherwise.
// Writes the given cText line to the file followed by a newline.
// Does nothing if SD card is not initialized or file open fails.
void WriteToSD(const char* cFileName, const char* cText, bool bAppend) {
  SafeSDAccess([&]() {
    if (!g_bIsSDInit)
      return;

    char cFinalFileName[64];

    if (strcmp(cFileName, "logging_") == 0) {
      struct tm currentTime = GetLocalTimeNow();
      snprintf(cFinalFileName, sizeof(cFinalFileName), "logging_%04d_%02d_%02d.log", currentTime.tm_year + 1900, currentTime.tm_mon + 1, currentTime.tm_mday);
    } else {
      strncpy(cFinalFileName, cFileName, sizeof(cFinalFileName));
      cFinalFileName[sizeof(cFinalFileName) - 1] = '\0';
    }

    File pFile = SD.open(cFinalFileName, bAppend ? FILE_APPEND : FILE_WRITE);
    if (pFile) {
      pFile.println(cText);

      pFile.close();
    }
  });
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Saves the profile settings of the given profile index (nProfile) to the SD card safely using SafeSDAccess.
// Profile data is saved to a file named "/veg", "/flo", or "/dry" depending on the profile index (0, 1, or 2).
// Writes all relevant profile parameters line by line.
// Additionally, saves the watering stages to a separate file with the suffix "_watering".
// Does nothing if the SD card is not initialized or file open fails.
// Logs a success message upon successful save.
void SaveProfile(uint8_t nProfile) {
  SafeSDAccess([&]() {
    if (!g_bIsSDInit)
      return;

    String strProfileName = ((nProfile == 0) ? "/veg" : ((nProfile == 1) ? "/flo" : "/dry"));
    File pProfileFile = SD.open(strProfileName, FILE_WRITE);  // Save Current Profile Values
    if (pProfileFile) {
      pProfileFile.println(g_pProfileSettings[nProfile].StartLightTime);
      pProfileFile.println(g_pProfileSettings[nProfile].StopLightTime);
      pProfileFile.println(g_pProfileSettings[nProfile].LightBrightness);

      pProfileFile.println(g_pProfileSettings[nProfile].StartInternalFanTemperature);

      pProfileFile.println(g_pProfileSettings[nProfile].StartVentilationTemperature);
      pProfileFile.println(g_pProfileSettings[nProfile].StartVentilationHumidity);

      pProfileFile.println(g_pProfileSettings[nProfile].PHReducerToApply);
      pProfileFile.println(g_pProfileSettings[nProfile].VegetativeFertilizerToApply);
      pProfileFile.println(g_pProfileSettings[nProfile].FloweringFertilizerToApply);

      pProfileFile.println(g_pProfileSettings[nProfile].IrrigationDayCounter);
      pProfileFile.println(g_pProfileSettings[nProfile].LastWateredDay);
      pProfileFile.println(g_pProfileSettings[nProfile].LastWateredHour);

      pProfileFile.close();
      ///////////////////////////////////////////////////
      File pWateringProfileFile = SD.open(strProfileName + "_watering", FILE_WRITE);
      if (pWateringProfileFile) {
        for (const auto& Watering : g_pProfileSettings[nProfile].WateringStages)
          pWateringProfileFile.printf("%u|%u\n", Watering.Day, Watering.TargetCC);

        pWateringProfileFile.close();
      }

      LOGGER(INFO, "Profile: %s updated successfully.", strProfileName);
    }
  });
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Saves current global configuration settings to the "/settings" file on the SD card using SafeSDAccess.
// Writes one setting per line, including Wi-Fi credentials, sampling interval, current profile,
// fan intervals, hysteresis values, flow rates, mixing pump duration, and irrigation reservoir level.
// If the SD card is not initialized or the file fails to open, the function does nothing.
// Logs a success message if the settings file is successfully written.
void SaveSettings() {
  SafeSDAccess([&]() {
    if (!g_bIsSDInit)
      return;

    File pSettingsFile = SD.open("/settings", FILE_WRITE);
    if (pSettingsFile) {
      pSettingsFile.println(g_cSSID);
      pSettingsFile.println(g_cSSIDPWD);

      pSettingsFile.println(g_nSamplingInterval);

      pSettingsFile.println(g_nCurrentProfile);

      pSettingsFile.println(g_nFansRestInterval);
      pSettingsFile.println(g_nFansRestDuration);

      pSettingsFile.println(g_nTemperatureStopHysteresis);
      pSettingsFile.println(g_nHumidityStopHysteresis);

      pSettingsFile.println(g_nIrrigationFlowPerMinute);
      pSettingsFile.println(g_nPHReducerFlowPerMinute);
      pSettingsFile.println(g_nVegetativeFlowPerMinute);
      pSettingsFile.println(g_nFloweringFlowPerMinute);

      pSettingsFile.println(g_nMixingPumpDuration);

      pSettingsFile.println(g_nIrrigationReservoirLowerLevel);

      pSettingsFile.close();

      LOGGER(INFO, "Settings file updated successfully.");
    }
  });
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Loads all defined profile settings from the SD card using SafeSDAccess.
// For each profile (veg, flo, dry), reads configuration values from the corresponding file:
// light schedule, brightness, fan/ventilation thresholds, irrigation day,
// and nutrient quantities to apply. Also loads per-day irrigation volumes from
// a secondary "_watering" file associated with each profile.
// Trims trailing whitespace and ensures that effective light start/stop times are updated
// for the current active profile.
// If files are missing or malformed, partial data may be loaded; does nothing if SD is not initialized.
void LoadProfiles() {
  SafeSDAccess([&]() {
    if (!g_bIsSDInit)
      return;

    char cBuffer[64];

    for (uint8_t i = 0; i < MAX_PROFILES; i++) {
      String strProfileName = ((i == 0) ? "/veg" : ((i == 1) ? "/flo" : "/dry"));

      File pProfileFile = SD.open(strProfileName, FILE_READ);
      if (pProfileFile) {
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // START LIGHT TIME
        g_pProfileSettings[i].StartLightTime = atoi(cBuffer);

        if (g_nCurrentProfile == i) // Only if the current loop corresponds to the current profile
          g_nEffectiveStartLights = (g_pProfileSettings[g_nCurrentProfile].StartLightTime == 24) ? 0 : g_pProfileSettings[g_nCurrentProfile].StartLightTime;  // Stores the effective light start hour, converting 24 to 0 (midnight)
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // STOP LIGHT TIME
        g_pProfileSettings[i].StopLightTime = atoi(cBuffer);

        if (g_nCurrentProfile == i) // Only if the current loop corresponds to the current profile
          g_nEffectiveStopLights = (g_pProfileSettings[g_nCurrentProfile].StopLightTime == 24) ? 0 : g_pProfileSettings[g_nCurrentProfile].StopLightTime; // Stores the effective light stop hour, converting 24 to 0 (midnight)
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // LIGHT BRIGHTNESS LEVEL
        g_pProfileSettings[i].LightBrightness = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // INTERNAL FAN TEMPERATURE START
        g_pProfileSettings[i].StartInternalFanTemperature = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // VENTILATION TEMPERATURE START
        g_pProfileSettings[i].StartVentilationTemperature = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // VENTILATION HUMIDITY START
        g_pProfileSettings[i].StartVentilationHumidity = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // CC OF PH REDUCER TO APPLY TO IRRIGATE SOLUTION
        g_pProfileSettings[i].PHReducerToApply = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // CC OF VEGETATIVE FERTILIZER TO APPLY TO IRRIGATE SOLUTION
        g_pProfileSettings[i].VegetativeFertilizerToApply = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // CC OF FLOWERING FERTILIZER TO APPLY TO IRRIGATE SOLUTION
        g_pProfileSettings[i].FloweringFertilizerToApply = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // IRRIGATION DAYS COUNTER
        g_pProfileSettings[i].IrrigationDayCounter = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // LAST IRRIGATION EXECUTE DAY
        g_pProfileSettings[i].LastWateredDay = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // LAST IRRIGATION EXECUTE HOUR
        g_pProfileSettings[i].LastWateredHour = atoi(cBuffer);

        pProfileFile.close();
      }
      ///////////////////////////////////////////////////
      File pWateringProfileFile = SD.open(strProfileName + "_watering", FILE_READ);
      if (pWateringProfileFile) {
        g_pProfileSettings[i].WateringStages.clear();

        while (pWateringProfileFile.available()) {
          cBuffer[pWateringProfileFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0';  // CC OF SOLUTION TO IRRIGATE
          TrimTrailingWhitespace(cBuffer);

          char* cDivider = strchr(cBuffer, '|');
          if (cDivider) {
            *cDivider = '\0';

            g_pProfileSettings[i].WateringStages.push_back({atoi(cBuffer), atoi(cDivider + 1)});
          }
        }

        pWateringProfileFile.close();
      }
    }
  });
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sends a WhatsApp notification message using the CallMeBot API.
// Encodes the message text for URL transmission by percent-encoding special characters.
// Constructs the full API request URL including phone number, encoded text, and API key.
// Performs an HTTP GET request to send the notification.
// Uses the HTTPClient library for making the HTTPS request.
void SendNotification(const char* strMessage) {
  HTTPClient http;
  size_t i, j = 0;
  char cEncodedMessage[512] = {0};

  for (i = 0; strMessage[i] != '\0' && j < sizeof(cEncodedMessage) - 4; ++i) {
    char c = strMessage[i];

    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      cEncodedMessage[j++] = c;
    } else {
      snprintf(&cEncodedMessage[j], 4, "%%%02X", (unsigned char)c);

      j += 3;
    }
  }

  cEncodedMessage[j] = '\0';

  char cFinalUrl[512];
  snprintf(cFinalUrl, sizeof(cFinalUrl), "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s", CALLMEBOT_PHONE_TO_SEND, cEncodedMessage, CALLMEBOT_APY_KEY);

  http.begin(cFinalUrl);
  http.setTimeout(3000);

  uint16_t nReturnCode = http.GET();
  if (nReturnCode == 200)
    LOGGER(INFO, "Notification sent successfully.");
  else
    LOGGER(ERROR, "Error while sending notification. Error Code: %d", nReturnCode);

  http.end();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Measures the irrigation reservoir level using an HC-SR04 ultrasonic sensor.
// Performs multiple distance readings to improve accuracy, averaging only the valid results.
// Each reading triggers the sensor and waits for the echo, discarding timeouts.
// Returns the average distance (in centimeters) as an integer (int16_t) from the sensor to the water surface.
// If all readings fail, logs an error and returns 0.
int16_t GetIrrigationReservoirLevel() {
  float fCombinedValues = 0;
  uint8_t nValidReads = 0;

  for (uint8_t i = 0; i < HCSR04_MAX_READS; i++) {
    if (!digitalRead(HCSR04_TRIGGER_PIN))
      digitalWrite(HCSR04_TRIGGER_PIN, HIGH);

    delayMicroseconds(10);  // Small Wait to obtain an stable reading

    digitalWrite(HCSR04_TRIGGER_PIN, LOW);

    float fDuration = pulseIn(HCSR04_ECHO_PIN, HIGH, 23529.4);

    if (fDuration == 0) {
      LOGGER(ERROR, "Irrigation Solution Level read out of range.");
    } else {
      fCombinedValues += fDuration * 0.0343 / 2;
      nValidReads++;
    }

    delay(100); // Small delay between reads
  }

  if (nValidReads > 0) {
    return static_cast<uint16_t>(fCombinedValues / nValidReads);
  } else {
    LOGGER(ERROR, "All HCSR04 readings failed.");

    return 0;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads the average humidity value from a soil sensor. 
// Excites the sensor by setting the VCC pin high and waits for stable readings. 
// Takes multiple analog readings, sums them, and normalizes the result to a percentage scale (0~100). 
// After readings, the VCC pin is set low to stop electrolysis. 
// Returns the normalized humidity value as an integer between 0 and 100.
uint8_t GetSoilHumidity(uint8_t nSensorNumber) {
  if (!digitalRead(HW080_VCC_PIN))
    digitalWrite(HW080_VCC_PIN, HIGH);  // Put Pin output in High to excite the moisture sensors

  delay(10);  // Small Wait to obtain an stable reading

  uint32_t nCombinedValues = 0;

  for (uint8_t i = 0; i < HW080_MAX_READS; i++) {
    nCombinedValues += analogRead(pSoilMoisturePins[nSensorNumber].Pin);

    delay(100); // Small delay between reads
  }

  digitalWrite(HW080_VCC_PIN, LOW); // Put pin output in Low to stop electrolysis

  return constrain(map(nCombinedValues / HW080_MAX_READS, HW080_MIN, HW080_MAX, 0, 100), 0, 100);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Handles automatic WiFi reconnection using the global SSID and password credentials.
// If ENABLE_AP_ALWAYS is not defined:
// - Starts a temporary Access Point (ACCESSPOINT_NAME) to allow reconfiguration during reconnection attempts.
// - Once connected, the Access Point is shut down and the mode is switched to station-only.
// Tries to reconnect up to WIFI_MAX_RETRYS times, with a 1-second delay between attempts.
// Logs a success message with IP address upon connection, or an error message if all attempts fail.
// After completion (regardless of success), the task is suspended until explicitly resumed elsewhere.
void Thread_WifiReconnect(void* parameter) {
  for (;;) {
#if !defined(ENABLE_AP_ALWAYS)
    if (!(WiFi.getMode() & WIFI_AP)) {
      LOGGER(INFO, "Starting Access Point (SSID: %s) mode for reconfiguration...", ACCESSPOINT_NAME);

      WiFi.mode(WIFI_AP_STA); // Set dual mode, Access Point & Station

      vTaskDelay(100 / portTICK_PERIOD_MS); // Delay to stabilize AP

      WiFi.softAP(ACCESSPOINT_NAME); // Start Access Point, while try to connect to Wifi
    }
#endif

    LOGGER(INFO, "Trying to reconnect Wifi...");

    WiFi.begin(g_cSSID, g_cSSIDPWD);

    uint8_t nConnectTrysCount = 0;

    while (WiFi.status() != WL_CONNECTED && nConnectTrysCount < WIFI_MAX_RETRYS) {
      nConnectTrysCount++;
      vTaskDelay(WIFI_CHECK_INTERVAL / portTICK_PERIOD_MS);  // Wait before trying again
    }

    if (WiFi.status() == WL_CONNECTED) {
      LOGGER(INFO, "Connected to Wifi SSID: %s PASSWORD: %s. IP: %s.", g_cSSID, g_cSSIDPWD, WiFi.localIP().toString().c_str());

#if !defined(ENABLE_AP_ALWAYS)
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);

      LOGGER(INFO, "Access Point disconnected.");
#endif
    } else {
      LOGGER(ERROR, "Max Wifi reconnect attempts reached.");
    }

    vTaskSuspend(NULL); // Suspends the task until needed again
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Generates and returns dynamic HTML content or variable values based on the input key `var`. Just for the first web request.
// Supports returning environmental data, system states, and dynamically generated HTML blocks
// for insertion into templates. Used with templating engines (e.g., AsyncWebServer's .setTemplateProcessor).
String HTMLProcessor(const String &var) {
  if (var == "IFPM") {
    return String(g_nIrrigationFlowPerMinute);
  } else if (var == "PHFPM") {
    return String(g_nPHReducerFlowPerMinute);
  } else if (var == "VEGFPM") {
    return String(g_nVegetativeFlowPerMinute);
  } else if (var == "FLOFPM") {
    return String(g_nFloweringFlowPerMinute);
  } else if (var == "MIXDUR") {
    return String(TicksToSeconds(g_nMixingPumpDuration));
  } else if (var == "SAINT") {
    return String(TicksToMinutes(g_nSamplingInterval));
  } else if (var == "ENVTEMP") {
    return String(g_nEnvironmentTemperature);
  } else if (var == "ENVHUM") {
    return String(g_nEnvironmentHumidity);
  } else if (var == "LEVEL") {
    return String(g_nIrrigationSolutionLevel) + "&#37;";
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

    return "<span id=vpd style=color:" + strHTMLColor + ">" + String(g_fEnvironmentVPD, 2) + "</span>kPa (<span id=vpdstate style=color:" + strHTMLColor + ">" + strState + "</span>)";
  } else if (var == "SOILSECTION") {
    String strReturn;

    for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++)
      strReturn += "<tr><td>Humedad de Maceta " + String(i) + ":</td><td><p id=soil" + String(i) + ">" + String(g_nSoilsHumidity[i]) + "&#37;</p></td></tr>";

    return strReturn;
  } else if (var == "CURRENTTIME") {
    struct tm currentTime = GetLocalTimeNow();
    auto timeformat = [](int value) { return (String(value).length() == 1) ? "0" + String(value) : String(value); };

    return String(timeformat(currentTime.tm_hour) + ":" + timeformat(currentTime.tm_min) + ":" + timeformat(currentTime.tm_sec));
  } else if (var == "TIMEZONE") {
    return String(TIMEZONE);
  } else if (var == "PROFILE") {
    return String(g_nCurrentProfile);
  } else if (var == "STARTLIGHT") {
    return String(g_pProfileSettings[g_nCurrentProfile].StartLightTime);
  } else if (var == "STOPLIGHT") {
    return String(g_pProfileSettings[g_nCurrentProfile].StopLightTime);
  } else if (var == "MAXBRIGHT") {
    return String(S8050_MAX_VALUE);
  } else if (var == "BRIGHTLEVEL") {
    return String((g_pProfileSettings[g_nCurrentProfile].LightBrightness < 401) ? 0 : g_pProfileSettings[g_nCurrentProfile].LightBrightness); // WARNING: Hardcode offset
  } else if (var == "STAINTFAN") {
    return String(g_pProfileSettings[g_nCurrentProfile].StartInternalFanTemperature);
  } else if (var == "TEMPSTARTVENT") {
    return String(g_pProfileSettings[g_nCurrentProfile].StartVentilationTemperature);
  } else if (var == "HUMSTARTVENT") {
    return String(g_pProfileSettings[g_nCurrentProfile].StartVentilationHumidity);
  } else if (var == "PHCC") {
    return String(g_pProfileSettings[g_nCurrentProfile].PHReducerToApply);
  } else if (var == "VEGCC") {
    return String(g_pProfileSettings[g_nCurrentProfile].VegetativeFertilizerToApply);
  } else if (var == "FLOCC") {
    return String(g_pProfileSettings[g_nCurrentProfile].FloweringFertilizerToApply);
  } else if (var == "IDC") {
    return String(g_pProfileSettings[g_nCurrentProfile].IrrigationDayCounter);
  } else if (var == "WATSTATE") {
    String strReturn;

    if (g_nIrrigationDuration > 0) {
      strReturn = "Regando...<br>Tiempo Restante: ";

      if (g_nIrrigationDuration < 60) {
        strReturn += String(g_nIrrigationDuration) + " segundos";
      } else {
        uint16_t nMinutes = g_nIrrigationDuration / 60;
        uint16_t nSeconds = g_nIrrigationDuration % 60;

        strReturn += String(nMinutes) + ((nMinutes == 1) ? " minuto" : " minutos");

        if (nSeconds > 0)
          strReturn += " y " + String(nSeconds) + " segundos";
      }
    }

    return strReturn;
  } else if (var == "RESTSTATE") {
    String strReturn;

    if (g_nFansRestElapsedTime > 0) {
      strReturn = "En Reposo...<br>Tiempo Restante: ";

      uint32_t nTimeRemaining = g_nFansRestDuration - (millis() - g_nFansRestElapsedTime);  // Miliseconds

      if (nTimeRemaining < 60000) {
        strReturn += String(nTimeRemaining / 1000) + " segundos";
      } else {
        uint32_t nMinutes = nTimeRemaining / 60000;
        uint32_t nSeconds = (nTimeRemaining % 60000) / 1000;

        strReturn += String(nMinutes) + (nMinutes == 1 ? " minuto" : " minutos");

        if (nSeconds > 0)
          strReturn += " y " + String(nSeconds) + " segundos";
      }
    }

    return strReturn;
  } else if (var == "RESTINTERVAL") {
    return String(TicksToMinutes(g_nFansRestInterval));
  } else if (var == "RESTDUR") {
    return String(TicksToMinutes(g_nFansRestDuration));
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

    for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++)
      strReturn += "{label:'Humedad de Maceta " + String(i) + "',borderColor:'" + pSoilMoisturePins[i].HTMLColor + "',backgroundColor:'" + pSoilMoisturePins[i].HTMLColor + "',symbol:'%%',yAxisID:'" + (3 + i) + "'},";

    return strReturn;
  }

  return String();
}

void setup() {
#ifdef ENABLE_SERIAL_LOGGER
  Serial.begin(9600);
  delay(3000);  // Small delay cuz that trash don't print the initial log
#endif

  LOGGER(INFO, "========== Indoor Controller Started ==========");
  LOGGER(INFO, "Initializing Pins...");

  for (uint8_t i = 0; i < nRelayModulePinsCount; ++i) {
    pinMode(pRelayModulePins[i].Pin, OUTPUT); // Set Pin Mode
    digitalWrite(pRelayModulePins[i].Pin, RELAY_PIN_OFF); // Set default Pin State

    LOGGER(INFO, "%s Pin Done!", pRelayModulePins[i].Name);
  }

  ledcAttach(S8050_PWM_PIN, S8050_FREQUENCY, S8050_RESOLUTION);
  LOGGER(INFO, "Light Brightness Pin Done!");

  pinMode(HW080_VCC_PIN, OUTPUT);
  digitalWrite(HW080_VCC_PIN, LOW);
  LOGGER(INFO, "Power Pin for Soil Humidity Sensors Done!");

  for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++) {
    pinMode(pSoilMoisturePins[i].Pin, INPUT);

    LOGGER(INFO, "Soil Humidity Pin %d Done!", i);
  }

  pinMode(HCSR04_TRIGGER_PIN, OUTPUT);
  digitalWrite(HCSR04_TRIGGER_PIN, LOW);
  pinMode(HCSR04_ECHO_PIN, INPUT);
  LOGGER(INFO, "Pins for Irrigation Solution Reservoir Level Done!");

  g_SDMutex = xSemaphoreCreateMutex();

  SafeSDAccess([&]() {  // Try to init SD Card
    LOGGER(INFO, "Loading Settings & Time...");

    if (g_bIsSDInit) {
      File pSettingsFile = SD.open("/settings", FILE_READ); // Read Settings File // NOTE: It is assumed that there will always be a settings file in the root
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
        cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // SAMPLING TAKE INTERVALS FOR GRAPH
        g_nSamplingInterval = atoi(cBuffer);
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
        cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // TEMPERATURE HYSTERESIS TO STOP FANS
        g_nTemperatureStopHysteresis = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // HUMIDITY HYSTERESIS TO STOP FANS
        g_nHumidityStopHysteresis = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // IRRIGATION PUMP FLOW PER MINUTE
        g_nIrrigationFlowPerMinute = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // PH REDUCER PUMP FLOW PER MINUTE
        g_nPHReducerFlowPerMinute = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // VEGETATIVE FERTILIZER PUMP FLOW PER MINUTE
        g_nVegetativeFlowPerMinute = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // FLOWERING FERTILIZER PUMP FLOW PER MINUTE
        g_nFloweringFlowPerMinute = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // MIXING PUMP DURATION
        g_nMixingPumpDuration = atoi(cBuffer);
        ///////////////////////////////////////////////////
        cBuffer[pSettingsFile.readBytesUntil('\n', cBuffer, sizeof(cBuffer) - 1)] = '\0'; // LOWER POINT OF IRRIGATION SOLUTION RESERVOIR
        g_nIrrigationReservoirLowerLevel = atoi(cBuffer);
        ///////////////////////////////////////////////////
        pSettingsFile.close();
        ///////////////////////////////////////////////////
        LoadProfiles();  // LOAD PROFILES VALUES

        ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - ((g_pProfileSettings[g_nCurrentProfile].LightBrightness < 401) ? 0 : g_pProfileSettings[g_nCurrentProfile].LightBrightness));  // WARNING: Hardcode offset
      } else {
        LOGGER(ERROR, "Failed to open Settings file.");
      }
      ///////////////////////////////////////////////////
      File pTimeFile = SD.open("/time", FILE_READ); // Read Time file // NOTE: It is assumed that there will always be a time file in the root
      if (pTimeFile) {
        LOGGER(INFO, "Getting Datetime from SD Card...");

        SetCurrentDatetime(pTimeFile.readStringUntil('\n').toInt());

        struct tm currentTime = GetLocalTimeNow();
        LOGGER(INFO, "Current Datetime: %04d-%02d-%02d %02d:%02d:%02d.", currentTime.tm_year + 1900, currentTime.tm_mon + 1, currentTime.tm_mday, currentTime.tm_hour, currentTime.tm_min, currentTime.tm_sec);

        pTimeFile.close();
      } else {
        LOGGER(ERROR, "Failed to open Time file.");
      }
      ///////////////////////////////////////////////////
      File pMetricsFile = SD.open("/metrics.log", FILE_READ);  // Read Time file  // NOTE: It is assumed that there will always be a metrics.log file in the root
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

                strncpy(g_strArrayGraphData[nLinesRead], cBuffer, MAX_GRAPH_MARKS_LENGTH - 1);
                g_strArrayGraphData[nLinesRead][MAX_GRAPH_MARKS_LENGTH - 1] = '\0';

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
      LOGGER(ERROR, "SD initialization failed. Settings & Time will not be loaded, but the system will not restart to avoid unexpected relay behavior.");
    }
  }, true);

  LOGGER(INFO, "Initializing Wifi...");

#ifdef ENABLE_AP_ALWAYS
  WiFi.mode(WIFI_AP_STA); // Set dual mode, Access Point & Station

  WiFi.softAP(ACCESSPOINT_NAME);

  LOGGER(INFO, "Access Point active. AP IP: %s", WiFi.softAPIP().toString().c_str());
#else
  WiFi.mode(WIFI_STA);  // Only Station mode
#endif

  if (g_bIsSDInit) {
    WiFi.begin(g_cSSID, g_cSSIDPWD);

    uint8_t nConnectTrysCount = 0;

    while (WiFi.status() != WL_CONNECTED && nConnectTrysCount < WIFI_MAX_RETRYS) {
      nConnectTrysCount++;
      delay(WIFI_CHECK_INTERVAL); // Wait before trying again
    }

    if (WiFi.status() == WL_CONNECTED)
      LOGGER(INFO, "Connected to Wifi SSID: %s PASSWORD: %s. IP: %s.", g_cSSID, g_cSSIDPWD, WiFi.localIP().toString().c_str());
    else
      LOGGER(ERROR, "Max Wifi reconnect attempts reached.");
  }

  LOGGER(INFO, "Initializing mDNS...");

  if (MDNS.begin(DNS_ADDRESS)) {
    MDNS.addService("http", "tcp", WEBSERVER_PORT);

    LOGGER(INFO, "mDNS Started at: %s.local Service: HTTP, Protocol: TCP, Port: %d.", DNS_ADDRESS, WEBSERVER_PORT);
  } else {
    LOGGER(ERROR, "Failed to initialize mDNS.");
  }

  LOGGER(INFO, "Creating Wifi reconnect task thread...");

  xTaskCreatePinnedToCore(Thread_WifiReconnect, "Wifi Reconnect Task", 4096, NULL, 1, &g_pWiFiReconnect, 0);
  vTaskSuspend(g_pWiFiReconnect); // Suspend the task as it's not needed right now

  LOGGER(INFO, "Setting up Web Server Paths & Commands...");

  for (uint8_t i = 0; i < sizeof(g_strWebServerFiles) / sizeof(g_strWebServerFiles[0]); i++) {
    String path = "/" + String(g_strWebServerFiles[i]);

    g_pWebServer.serveStatic(path.c_str(), SD, path.c_str()).setCacheControl("max-age=86400");
  }

  g_pWebServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    /*IPAddress pClientIP = request->client()->getRemoteAddress();
    LOGGER(INFO, "HTTP Request IP From: %d.%d.%d.%d.", pClientIP[0], pClientIP[1], pClientIP[2], pClientIP[3]);*/

    if (request->hasArg("action")) {  // Process the request
      if (request->arg("action") == "cisl") {
        g_nIrrigationReservoirLowerLevel = GetIrrigationReservoirLevel();

        SaveSettings();

        request->send(200, "text/plain", "MSGSe calibró el valor Mínimo del Reservorio de Solución de Riego.");
      } else if (request->arg("action") == "reload") {  // This is for reload Elements from the Panel based on selected Profile
        String strReturn = "";

        if (request->hasArg("profilesel")) {
          uint8_t nSelectedProfile = request->arg("profilesel").toInt();

          /*strReturn += "lightstart:" + String(g_pProfileSettings[nSelectedProfile].StartLightTime); // Previous version (Inputs)
          strReturn += ":lightstop:" + String(g_pProfileSettings[nSelectedProfile].StopLightTime);*/
          strReturn += "lightstartstop:" + String(g_pProfileSettings[nSelectedProfile].StartLightTime) + "," + String(g_pProfileSettings[nSelectedProfile].StopLightTime);  // Newest version (Scroll Wheel Selector)
          strReturn += ":lightbright:" + String(g_pProfileSettings[nSelectedProfile].LightBrightness);
          ///////////////////////////////////////////////////
          strReturn += ":intfansta:" + String(g_pProfileSettings[nSelectedProfile].StartInternalFanTemperature);
          ///////////////////////////////////////////////////
          strReturn += ":venttempstart:" + String(g_pProfileSettings[nSelectedProfile].StartVentilationTemperature);
          strReturn += ":venthumstart:" + String(g_pProfileSettings[nSelectedProfile].StartVentilationHumidity);
          ///////////////////////////////////////////////////
          strReturn += ":phcc:" + String(g_pProfileSettings[nSelectedProfile].PHReducerToApply);
          strReturn += ":vegcc:" + String(g_pProfileSettings[nSelectedProfile].VegetativeFertilizerToApply);
          strReturn += ":flocc:" + String(g_pProfileSettings[nSelectedProfile].FloweringFertilizerToApply);
          ///////////////////////////////////////////////////
          strReturn += ":idc:" + String(g_pProfileSettings[nSelectedProfile].IrrigationDayCounter);

          bool bFirst = true;
          strReturn += ":wateringchart:";

          for (const auto& Watering : g_pProfileSettings[nSelectedProfile].WateringStages) {
            if (!bFirst)
              strReturn += ",";
            else
              bFirst = false;

            strReturn += String(Watering.Day) + "|" + String(Watering.TargetCC);
          }
        }

        if (strReturn != "")
          request->send(200, "text/plain", "RELOAD" + strReturn);
      } else if (request->arg("action") == "applyferts") {
        String strReturn = "No se pueden incorporar los Fertilizantes en este momento, ";

        if (g_nTestPumpStartTime == 0 && g_nIrrigationDuration == 0) {
          strReturn = "Se comenzará a incorporar los fertilizantes.";

          g_bApplyFertilizers = true;
        } else {
          if (g_nTestPumpStartTime > 0)
            strReturn += "porque se está ejecutando una prueba de Caudal.";
          else if (g_nIrrigationDuration > 0)
            strReturn += "porque se está ejecutando un Riego.";
        }

        request->send(200, "text/plain", "MSG" + strReturn);
      } else if (request->arg("action") == "testIpump" || // Irrigation
                 request->arg("action") == "testPHpump" || // pH Reducer
                 request->arg("action") == "testVpump" ||  // Vegetative Fert
                 request->arg("action") == "testFpump") {  // Flowering Fert
        String strReturn = "La bomba no se puede probar en este momento.";

        if (g_nTestPumpStartTime == 0 && g_nIrrigationDuration == 0) {
          if (request->arg("action") == "testIpump")
            g_bTestIrrigationPump = true;
          else if (request->arg("action") == "testPHpump")
            g_bTestPHReducerPump = true;
          else if (request->arg("action") == "testVpump")
            g_bTestVegetativeFertPump = true;
          else if (request->arg("action") == "testFpump")
            g_bTestFloweringFertPump = true;

          if (g_bTestIrrigationPump || g_bTestPHReducerPump || g_bTestVegetativeFertPump || g_bTestFloweringFertPump)
            strReturn = "La bomba estará encendida durante los próximos 60 segundos.";
        }

        request->send(200, "text/plain", "MSG" + strReturn);
      } else if (request->arg("action") == "update") {  // This is for update Settings
        uint8_t nSelectedProfile = request->arg("profilesel").toInt();
        uint64_t nNewValue;
        String strReturn = "";

        // =============== Current DateTime =============== //
        if (request->hasArg("settime")) {
          SetCurrentDatetime(request->arg("settime").toInt());

          struct tm currentTime = GetLocalTimeNow();
          LOGGER(INFO, "New Datetime: %04d-%02d-%02d %02d:%02d:%02d.", currentTime.tm_year + 1900, currentTime.tm_mon + 1, currentTime.tm_mday, currentTime.tm_hour, currentTime.tm_min, currentTime.tm_sec);

          strReturn += "Se sincronizó la Fecha actual.\r\n";
        }
        // =============== Profile Selector =============== //
        if (request->hasArg("profilesel")) {
          if (nSelectedProfile != g_nCurrentProfile) {
            g_nCurrentProfile = nSelectedProfile;

            LoadProfiles();

            // Reset Watering variables
            g_pProfileSettings[nSelectedProfile].IrrigationDayCounter = 0;

            struct tm currentTime = GetLocalTimeNow();

            g_pProfileSettings[nSelectedProfile].LastWateredDay = currentTime.tm_mday;
            g_pProfileSettings[nSelectedProfile].LastWateredHour = currentTime.tm_hour;

            strReturn += "Se cambió al Perfil de ";
            strReturn += (nSelectedProfile == 0) ? "Vegetativo" : ((nSelectedProfile == 1) ? "Floración" : "Secado");
            strReturn += ".\r\nAdemás, se reinició el Contador de Días de Riegos transcurridos, se iniciará a Regar a partir de la siguiente hora.\r\n";
          }
        }
        // =============== Light Start =============== //
        if (request->hasArg("lightstart")) {
          nNewValue = request->arg("lightstart").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].StartLightTime) {
            g_pProfileSettings[nSelectedProfile].StartLightTime = nNewValue;
            g_nEffectiveStartLights = (g_pProfileSettings[nSelectedProfile].StartLightTime == 24) ? 0 : g_pProfileSettings[nSelectedProfile].StartLightTime;  // Stores the effective light start hour, converting 24 to 0 (midnight)

            strReturn += "Se actualizó la Hora de Encendido de Luz.\r\n";
          }
        }
        // =============== Light Stop =============== //
        if (request->hasArg("lightstop")) {
          nNewValue = request->arg("lightstop").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].StopLightTime) {
            g_pProfileSettings[nSelectedProfile].StopLightTime = nNewValue;
            g_nEffectiveStopLights = (g_pProfileSettings[nSelectedProfile].StopLightTime == 24) ? 0 : g_pProfileSettings[nSelectedProfile].StopLightTime; // Stores the effective light stop hour, converting 24 to 0 (midnight)

            strReturn += "Se actualizó la Hora de Apagado de Luz.\r\n";
          }
        }
        // =============== Light Brightness =============== //
        if (request->hasArg("lightbright")) {
          nNewValue = request->arg("lightbright").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].LightBrightness) {
            g_pProfileSettings[nSelectedProfile].LightBrightness = (nNewValue < 401) ? 0 : nNewValue;

            ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - g_pProfileSettings[nSelectedProfile].LightBrightness);

            strReturn += "Se actualizó la Intensidad de Luz.\r\n";
          }
        }
        // =============== Internal Fan Start =============== //
        if (request->hasArg("intfansta")) {
          nNewValue = request->arg("intfansta").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].StartInternalFanTemperature) {
            g_pProfileSettings[nSelectedProfile].StartInternalFanTemperature = nNewValue;

            strReturn += "Se actualizó la Temperatura de Encendido del Ventilador Interno.\r\n";
          }
        }
        // =============== Ventilation Temperature Start =============== //
        if (request->hasArg("venttempstart")) {
          nNewValue = request->arg("venttempstart").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].StartVentilationTemperature) {
            g_pProfileSettings[nSelectedProfile].StartVentilationTemperature = nNewValue;

            strReturn += "Se actualizó la Temperatura de Encendido de Recirculación.\r\n";
          }
        }
        // =============== Ventilation Humidity Start =============== //
        if (request->hasArg("venthumstart")) {
          nNewValue = request->arg("venthumstart").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].StartVentilationHumidity) {
            g_pProfileSettings[nSelectedProfile].StartVentilationHumidity = nNewValue;

            strReturn += "Se actualizó la Humedad de Encendido de la Recirculación.\r\n";
          }
        }
        // =============== Application of CC of pH Reducer =============== //
        if (request->hasArg("phcc")) {
          nNewValue = request->arg("phcc").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].PHReducerToApply) {
            g_pProfileSettings[nSelectedProfile].PHReducerToApply = nNewValue;

            strReturn += "Se actualizó la cantidad de Reductor de pH que se incorporará a la Solución de Riego.\r\n";
          }
        }
        // =============== Application of CC of Vegetative Fertilizer =============== //
        if (request->hasArg("vegcc")) {
          nNewValue = request->arg("vegcc").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].VegetativeFertilizerToApply) {
            g_pProfileSettings[nSelectedProfile].VegetativeFertilizerToApply = nNewValue;

            strReturn += "Se actualizó la cantidad de Fertilizante de Vegetativo que se incorporará a la Solución de Riego.\r\n";
          }
        }
        // =============== Application of CC of Flowering Fertilizer =============== //
        if (request->hasArg("flocc")) {
          nNewValue = request->arg("flocc").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].FloweringFertilizerToApply) {
            g_pProfileSettings[nSelectedProfile].FloweringFertilizerToApply = nNewValue;

            strReturn += "Se actualizó la cantidad de Fertilizante de Floración que se incorporará a la Solución de Riego.\r\n";
          }
        }
        // =============== Current Watering Day =============== //
        if (request->hasArg("idc")) {
          nNewValue = request->arg("idc").toInt();

          if (nNewValue != g_pProfileSettings[nSelectedProfile].IrrigationDayCounter) {
            g_pProfileSettings[nSelectedProfile].IrrigationDayCounter = nNewValue;

            strReturn += "Se actualizó los Días de Riego transcurridos.\r\n";
          }
        }
        // =============== Watering Chart =============== //
        if (request->hasArg("wateringchart")) {
          String strValues = request->arg("wateringchart");
          uint16_t nIndex = 0;
          std::vector<WateringData> vecNewWateringStages;

          while (nIndex < strValues.length()) {
            int16_t nCommaIndex = strValues.indexOf(',', nIndex);
            String strPair = (nCommaIndex == -1) ? strValues.substring(nIndex) : strValues.substring(nIndex, nCommaIndex);

            int16_t nSepIndex = strPair.indexOf('|');
            if (nSepIndex != -1)
              vecNewWateringStages.push_back({ strPair.substring(0, nSepIndex).toInt(), strPair.substring(nSepIndex + 1).toInt() });

            if (nCommaIndex == -1)
              break;

            nIndex = nCommaIndex + 1;
          }

          if (vecNewWateringStages != g_pProfileSettings[nSelectedProfile].WateringStages) {
            g_pProfileSettings[nSelectedProfile].WateringStages = vecNewWateringStages;

            strReturn += "Se actualizó el Esquema de Riego.\r\n";
          }
        }
        // =============== Fans Rest Interval =============== //
        if (request->hasArg("restint")) {
          nNewValue = MinutesToTicks(request->arg("restint").toInt());

          if (nNewValue != g_nFansRestInterval) {
            g_nFansRestInterval = nNewValue;

            strReturn += "Se actualizó el Intervalo de Reposo de Ventiladores.\r\n";
          }
        }
        // =============== Fans Rest Duration =============== //
        if (request->hasArg("restdur")) {
          nNewValue = MinutesToTicks(request->arg("restdur").toInt());

          if (nNewValue != g_nFansRestDuration) {
            g_nFansRestDuration = nNewValue;

            strReturn += "Se actualizó la Duración de Reposo de Ventiladores.\r\n";
          }
        }
        // =============== Environment Temperature Hysteresis =============== //
        if (request->hasArg("temphys")) {
          nNewValue = request->arg("temphys").toInt();

          if (nNewValue != g_nTemperatureStopHysteresis) {
            g_nTemperatureStopHysteresis = nNewValue;

            strReturn += "Se actualizó la Histéresis de Apagado por Temperatura.\r\n";
          }
        }
        // =============== Environment Humidity Hysteresis =============== //
        if (request->hasArg("humhys")) {
          nNewValue = request->arg("humhys").toInt();

          if (nNewValue != g_nHumidityStopHysteresis) {
            g_nHumidityStopHysteresis = nNewValue;

            strReturn += "Se actualizó la Histéresis de Apagado por Humedad.\r\n";
          }
        }
        // =============== Irrigation Pump CC Flow Per Minute =============== //
        if (request->hasArg("ifpm")) {
          nNewValue = request->arg("ifpm").toInt();

          if (nNewValue != g_nIrrigationFlowPerMinute) {
            g_nIrrigationFlowPerMinute = nNewValue;

            strReturn += "Se actualizó el Caudal por Minuto de Bomba de Riego.\r\n";
          }
        }
        // =============== pH Reducer Pump CC Flow Per Minute =============== //
        if (request->hasArg("phfpm")) {
          nNewValue = request->arg("phfpm").toInt();

          if (nNewValue != g_nPHReducerFlowPerMinute) {
            g_nPHReducerFlowPerMinute = nNewValue;

            strReturn += "Se actualizó el Caudal por Minuto de Bomba de Reductor de pH.\r\n";
          }
        }
        // =============== Vegetative Fertilizer CC Pump Flow Per Minute =============== //
        if (request->hasArg("vegfpm")) {
          nNewValue = request->arg("vegfpm").toInt();

          if (nNewValue != g_nVegetativeFlowPerMinute) {
            g_nVegetativeFlowPerMinute = nNewValue;

            strReturn += "Se actualizó el Caudal por Minuto de Bomba de Fertilizante de Vegetativo.\r\n";
          }
        }
        // =============== Flowering Fertilizer CC Pump Flow Per Minute =============== //
        if (request->hasArg("flofpm")) {
          nNewValue = request->arg("flofpm").toInt();

          if (nNewValue != g_nFloweringFlowPerMinute) {
            g_nFloweringFlowPerMinute = nNewValue;

            strReturn += "Se actualizó el Caudal por Minuto de Bomba de Fertilizante de Floración.\r\n";
          }
        }
        // =============== Mixing Pump Duration =============== //
        if (request->hasArg("mixdur")) {
          nNewValue = SecondsToTicks(request->arg("mixdur").toInt());

          if (nNewValue != g_nMixingPumpDuration) {
            g_nMixingPumpDuration = nNewValue;

            strReturn += "Se actualizó la Duración de Mezcla de Solución de Riego.\r\n";
          }
        }
        // =============== Sampling take Intervals =============== //
        if (request->hasArg("saint")) {
          nNewValue = MinutesToTicks(request->arg("saint").toInt());

          if (nNewValue != g_nSamplingInterval) {
            g_nSamplingInterval = nNewValue;

            strReturn += "Se actualizó el Intervalo de toma de Muestras.\r\n";
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

          SaveSettings();

          SaveProfile(nSelectedProfile);

          if (bWiFiChanges) { // After send response to web client, Try reconnect to Wifi if is required
            LOGGER(WARN, "Disconnecting Wifi to start connection to new SSID...");

            WiFi.disconnect(false); // First disconnect from current Network (Arg false to just disconnect the Station, not the AP)

            if (eTaskGetState(g_pWiFiReconnect) == eRunning)  // Then check if task g_pWiFiReconnect is running. If it is, Suspend it to eventually Resume it with the new SSID and SSID Password
              vTaskSuspend(g_pWiFiReconnect);
          }
        }
      } else if (request->arg("action") == "restart") {
        request->send(200, "text/plain", "MSGReiniciando Controlador...");

        LOGGER(INFO, "Restarting Controller by Web command.");

        delay(1000);

        ESP.restart();
      } else if (request->arg("action") == "refresh") { // This is for refresh Panel values
        uint8_t nSelectedProfile = request->arg("profilesel").toInt();
        String strResponse = "";
        // ================================================== Environment Section ================================================== //
        strResponse += String(g_nEnvironmentTemperature) + ":" + String(g_nEnvironmentHumidity) + ":" + String(g_fEnvironmentVPD, 2);
        // ================================================== Irrigation Solution Level Section ================================================== //
        strResponse += String(g_nIrrigationSolutionLevel) + "%";
        // ================================================== Soil Section ================================================== //
        strResponse += ":";
        bool bFirst = true;

        for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++) {
          if (!bFirst)
            strResponse += ",";
          else
            bFirst = false;

          strResponse += String(g_nSoilsHumidity[i]) + "%";
        }
        // ================================================== Current Time Section ================================================== //
        time_t timeNow = time(nullptr);

        strResponse += ":" + String(timeNow);
        // ================================================== Light Brightness slider position Section ================================================== //
        strResponse += ":" + String((g_pProfileSettings[nSelectedProfile].LightBrightness < 401) ? 0 : g_pProfileSettings[nSelectedProfile].LightBrightness);
        // ================================================== Fans Rest State Section ================================================== //
        strResponse += ":";

        if (g_bFansRest)
          strResponse += String(TicksToSeconds(g_nFansRestDuration - (millis() - g_nFansRestElapsedTime)));
        else
          strResponse += "0";

        // Internal Fan
        strResponse += ":" + String(digitalRead(GetPinByName("Internal Fan"))); // 1 = stopped, 0 turn on

        // Ventilation
        strResponse += ":" + String(digitalRead(GetPinByName("Ventilation")));  // 1 = stopped, 0 turn on
        // ================================================== Irrigation Section ================================================== //
        strResponse += ":" + String(g_pProfileSettings[nSelectedProfile].IrrigationDayCounter);
        strResponse += ":" + String(g_nIrrigationDuration);  // Seconds
        // ================================================== Graph Section ================================================== //
        if (g_strArrayGraphData[0][0] != '\0') {
          strResponse += ":";

          for (int8_t i = MAX_GRAPH_MARKS - 1; i >= 0; i--) {
            if (g_strArrayGraphData[i][0] != '\0') {
              if (i < (MAX_GRAPH_MARKS - 1))
                strResponse += ",";

              strResponse += String(g_strArrayGraphData[i]);
            }
          }
        }
        // ========================================================================================================================= //
        AsyncWebServerResponse* response = request->beginResponse(200, "text/plain;charset=utf-8", "REFRESH" + strResponse);
        /*
          Response structure example: each data[X] is divided by ':'
          data[0] → Environment Temperature
          data[1] → Environment Humidity
          data[2] → Environment VPD
          data[3] → Irrigation Solution Level
          data[4] → <Soil Moistures values Array> Example: SOIL 1 MOISTURE VALUE,SOIL 2 MOISTURE VALUE
          data[5] → Current Timestamp
          data[6] → Light Brightness
          data[7] → Fans Rest time Remaining
          data[8] → Internal Fan State
          data[9] → Ventilation Fan State
          data[10] → Irrigation Day Counter
          data[11] → Irrigation Time Remaining
          data[12] → <History Chart values Array> Example: Unix Timestamp|Environment Temperature|Environment Humidity|VPD|<Soil Moistures values Array>
        */
        request->send(response);
      }
    } else {  // Return Panel content
      SafeSDAccess([&]() {
        AsyncWebServerResponse* pResponse;

        if (g_bIsSDInit)
          pResponse = request->beginResponse(SD, "/index.html", "text/html", false, HTMLProcessor);
        else
          pResponse = request->beginResponse(500, "text/plain", "No hay una Tarjeta SD conectada.");

        request->send(pResponse);
      });
    }
  });

  g_pWebServer.on("/ota", HTTP_POST, [](AsyncWebServerRequest* request) {
    bool bUpdate = !Update.hasError();

    String strMessage = bUpdate ? "Actualización cargada correctamente. Reiniciando..." : "Error al intentar Actualizar.";
    strMessage.replace("'", "\\'");

    request->send(200, "text/html", "<script>alert('" + strMessage + "');</script>");

    if (bUpdate) {
      LOGGER(INFO, "Restarting Controller to do a Firmware Update.");

      delay(1000);

      ESP.restart();
    }
  }, [](AsyncWebServerRequest* request, String strFileName, size_t nIndex, uint8_t* nData, size_t nLen, bool bFinal) {
    if (!nIndex) {
      LOGGER(INFO, "Updating Firmwware. File: %s", strFileName);

      if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        LOGGER(ERROR, "Firmware update failed. Error: %s", Update.errorString());
    }

    if (Update.write(nData, nLen) != nLen)
      LOGGER(ERROR, "Firmware update failed. Error: %s", Update.errorString());

    if (bFinal) {
      if (Update.end(true))
        LOGGER(INFO, "Firmware Update successfully.");
      else
        LOGGER(ERROR, "Firmware update failed. Error: %s", Update.errorString());
    }
  });

  g_pWebServer.begin();

  LOGGER(INFO, "Web Server Started at Port: %d.", WEBSERVER_PORT);
}

void loop() {
  static uint32_t nLastSecondTick = 0;  // General
  static uint32_t nLastStoreElapsedTime = 0;  // To Save Environment values to show in Graph
  uint64_t nCurrentMillis = millis();

  if ((nCurrentMillis - nLastSecondTick) >= 1000) { // Check if 1 second has passed since the last tick to perform once-per-second tasks
    nLastSecondTick = nCurrentMillis;
    time_t timeNow = time(nullptr);
    struct tm timeInfo;
    localtime_r(&timeNow, &timeInfo);
    // ================================================== Wifi Section ================================================== //
    if (eTaskGetState(g_pWiFiReconnect) == eSuspended && WiFi.status() != WL_CONNECTED) // If is not connected to Wifi and is not currently running a reconnect trask, start it
      vTaskResume(g_pWiFiReconnect);
    // ================================================== Time Section ================================================== //
    {
      char cBuffer[12];
      snprintf(cBuffer, sizeof(cBuffer), "%lu", static_cast<uint32_t>(timeNow));
      WriteToSD("/time", cBuffer, false); // Write current time to SD Card
    }
    // ================================================== Environment Section ================================================== //
    {
      uint8_t nError = SimpleDHTErrSuccess;
      uint8_t nReadTrysCount = 0;
      byte bTemperature = 0, bHumidity = 0;

      do {  // Try read Temp & Humidity values from Environment
        if ((nError = g_pDHT11.read(&bTemperature, &bHumidity, NULL)) != SimpleDHTErrSuccess) { // Get Environment Temperature and Humidity
          nReadTrysCount++;

          LOGGER(ERROR, "Read DHT11 failed, Error=%d, Duration=%d.", SimpleDHTErrCode(nError), SimpleDHTErrDuration(nError));

          delay(100);
        }
      } while (nError != SimpleDHTErrSuccess && nReadTrysCount < DHT_MAX_READS);  // Repeat while it fails and not reach max trys

      if (nError == SimpleDHTErrSuccess) {
        g_nEnvironmentTemperature = static_cast<uint8_t>(bTemperature);
        g_nEnvironmentHumidity = static_cast<uint8_t>(bHumidity);

        if ((g_nEnvironmentTemperature >= 0 && g_nEnvironmentTemperature <= 52) && (g_nEnvironmentHumidity >= 15 && g_nEnvironmentHumidity <= 95)) {  // WARNING: DHT11 → Temp 0~50±2 | Hum 0~90±5
          float fE_s = 6.112 * exp((17.67 * g_nEnvironmentTemperature) / (243.5 + g_nEnvironmentTemperature));

          g_fEnvironmentVPD = (fE_s - (g_nEnvironmentHumidity / 100.0) * fE_s) / 10.0;
        } else {
          g_fEnvironmentVPD = 0;

          LOGGER(ERROR, "Invalid Temperature or Humidity readings.");
        }
      } else {
        g_nEnvironmentTemperature = 0;
        g_nEnvironmentHumidity = 0;
        g_fEnvironmentVPD = 0;

        LOGGER(ERROR, "Failed to read DHT11 after max retries.");
      }
    }
    // ================================================== Irrigation Solution Level Section ================================================== //
    {
      g_nIrrigationSolutionLevel = std::clamp(100 * (g_nIrrigationReservoirLowerLevel - GetIrrigationReservoirLevel()) / g_nIrrigationReservoirLowerLevel, 0, 100);
    }
    // ================================================== Lights Sections ================================================== //
    if ((g_pProfileSettings[g_nCurrentProfile].StartLightTime > 0 || g_pProfileSettings[g_nCurrentProfile].StopLightTime > 0) &&  // Check if either the light start time or stop time is set (greater than 0)
        (g_nEffectiveStartLights < g_nEffectiveStopLights && timeInfo.tm_hour >= g_nEffectiveStartLights && timeInfo.tm_hour < g_nEffectiveStopLights) || // Normal case: light start time is before stop time (e.g., from 7 AM to 7 PM)
        (g_nEffectiveStartLights >= g_nEffectiveStopLights && (timeInfo.tm_hour >= g_nEffectiveStartLights || timeInfo.tm_hour < g_nEffectiveStopLights))) {  // Special case: light schedule crosses midnight (e.g., from 8 PM to 6 AM)
      if (digitalRead(GetPinByName("Lights"))) {
        digitalWrite(GetPinByName("Lights"), RELAY_PIN_ON);

        LOGGER(INFO, "Lights Started.");
      }
    } else {  // If Current Time is out of ON range, turn it OFF
      digitalWrite(GetPinByName("Lights"), RELAY_PIN_OFF);

      LOGGER(INFO, "Lights Stopped.");
    }
    // ================================================== Irrigation Section ================================================== //
    {
      static bool bIsTheLastPulse = false;

      if (((timeInfo.tm_hour - g_pProfileSettings[g_nCurrentProfile].LastWateredHour + 24) % 24) > 0 && !g_bApplyFertilizers && g_nTestPumpStartTime == 0) {
        static uint8_t nCurrentPulse = 0;
        static bool bApplyIrrigation = false;
        static uint8_t nCurrentPulseHour = 0;
        uint8_t nPulseInterval = g_nCurrentProfile == 1 /*Flowering*/ ? 2 : 1 /*Vegetative*/;
        uint8_t nStartIrrigationHour = (g_nEffectiveStartLights + 2) % 24;
        uint8_t nStopIrrigationHour = (g_nEffectiveStopLights - 2 + 24) % 24;
        uint8_t nTotalPulses = ((nStopIrrigationHour - nStartIrrigationHour + 24) % 24) / nPulseInterval;

        if (g_nIrrigationFlowPerMinute > 0 && nTotalPulses > 0) {
          if (!digitalRead(GetPinByName("Lights")) && !bApplyIrrigation) {
            uint16_t nLastKnownCC = 0;

            for (const auto& Watering : g_pProfileSettings[g_nCurrentProfile].WateringStages) {
              if (g_pProfileSettings[g_nCurrentProfile].IrrigationDayCounter >= Watering.Day)
                nLastKnownCC = Watering.TargetCC;
              else
                break;
            }

            for (nCurrentPulse = 0; nCurrentPulse < nTotalPulses; nCurrentPulse++) {
              nCurrentPulseHour = (nStartIrrigationHour + nCurrentPulse * nPulseInterval) % 24;

              if (nCurrentPulseHour == timeInfo.tm_hour) {
                bApplyIrrigation = true;
                g_nIrrigationDuration = ((static_cast<float>(nLastKnownCC) / nTotalPulses) * 60000) / g_nIrrigationFlowPerMinute;

                if (nCurrentPulse == (nTotalPulses - 1))
                  bIsTheLastPulse = true;

                break;
              }
            }
          } else if (bApplyIrrigation) {
            if (PowerSupplyControl(true)) {
              static uint32_t nIrrigationTimer = 0;

              if (nIrrigationTimer == 0) {
                nIrrigationTimer = nCurrentMillis;
              } else {
                static bool bWaitTime = false;

                if (!bWaitTime && (nCurrentMillis - nIrrigationTimer) >= 1500) {  // Wait 1.5 seconds to stabilize the voltage output just in case
                  bWaitTime = true;
                } else if (bWaitTime) {
                  static uint8_t nStage = 0;

                  switch (nStage) {
                    case 0: // Mixing Pump
                      {
                        const char* cPump = "Mixing Pump";

                        if (digitalRead(GetPinByName(cPump))) {
                          digitalWrite(GetPinByName(cPump), RELAY_PIN_ON);

                          nIrrigationTimer = nCurrentMillis;

                          LOGGER(INFO, "%s Started.", cPump);
                        } else {
                          if ((nCurrentMillis - nIrrigationTimer) >= g_nMixingPumpDuration /*Ticks*/) {
                            digitalWrite(GetPinByName(cPump), RELAY_PIN_OFF);

                            LOGGER(INFO, "%s Stopped.", cPump);

                            nStage++;
                          }
                        }
                      }
                      break;
                    case 1: // Irrigation Pump
                      {
                        const char* cPump = "Irrigation Pump";

                        if (digitalRead(GetPinByName(cPump))) {
                          digitalWrite(GetPinByName(cPump), RELAY_PIN_ON);

                          nIrrigationTimer = nCurrentMillis;

                          LOGGER(INFO, "%s Started. Irrigation Data: Pulse Number: %d/%d Current Hour: %d Pulse Duration: %d seconds.", cPump, (nCurrentPulse + 1), nTotalPulses, nCurrentPulseHour, TicksToSeconds(g_nIrrigationDuration));
                        } else {
                          if ((nCurrentMillis - nIrrigationTimer) >= g_nIrrigationDuration /*Ticks*/) {
                            digitalWrite(GetPinByName(cPump), RELAY_PIN_OFF);

                            LOGGER(INFO, "%s Stopped. Irrigation Finished", cPump);

                            PowerSupplyControl(false);

                            bWaitTime = false;
                            nStage = 0;
                            bApplyIrrigation = false;
                            g_pProfileSettings[g_nCurrentProfile].LastWateredHour = nCurrentPulseHour;
                          }
                        }
                      }
                      break;
                  }
                }
              }
            }
          }
        } else {
          LOGGER(ERROR, "The Irrigation Scheme or the Flow Rate per Minute of the Irrigation Pump was not defined, this Pulse will be skipped.");

          SendNotification(String("El Esquema de Riego o el Caudal por Minuto de la Bomba de Riego, no fue definido, se saltará este Pulso.").c_str());

          g_pProfileSettings[g_nCurrentProfile].LastWateredHour = nCurrentPulseHour;  // NOTE: Dilemma, this Pulse will be marked as irrigated. All you have to do is reconfigure the parameters and wait for the next one to begin watering
        }
      } else if (bIsTheLastPulse && timeInfo.tm_mday != g_pProfileSettings[g_nCurrentProfile].LastWateredDay && timeInfo.tm_hour == g_pProfileSettings[g_nCurrentProfile].LastWateredHour) {
        g_pProfileSettings[g_nCurrentProfile].IrrigationDayCounter++;
        g_pProfileSettings[g_nCurrentProfile].LastWateredDay = timeInfo.tm_mday;
        g_pProfileSettings[g_nCurrentProfile].LastWateredHour = -1;

        SaveProfile(g_nCurrentProfile);

        if (g_nIrrigationSolutionLevel <= 25) {
          char cBuffer[41];
          snprintf(cBuffer, sizeof(cBuffer), "Reservorio de Solución de Riego al %d%.", g_nIrrigationSolutionLevel);
          SendNotification(cBuffer);
        }

        bIsTheLastPulse = false;
      }
    }
    // ================================================== Fans Section ================================================== //
    if (!g_bFansRest && g_nFansRestInterval > 0) {  // If is not Rest time for Fans & check if g_nFansRestInterval just in case of SD is not connected at Controller startup
      static uint32_t nFansRestIntervalTime = 0;

      if ((nCurrentMillis - nFansRestIntervalTime) >= g_nFansRestInterval) {  // First check if need go in Rest time
        nFansRestIntervalTime = nCurrentMillis;
        g_nFansRestElapsedTime = nCurrentMillis;

        // Force Fans stop
        digitalWrite(GetPinByName("Internal Fan"), RELAY_PIN_OFF);
        digitalWrite(GetPinByName("Ventilation"), RELAY_PIN_OFF);

        g_bFansRest = true;

        LOGGER(INFO, "Fans Rest mode Started.");
      } else {  // Is not
        if (g_nEnvironmentTemperature > 0 && g_nEnvironmentHumidity > 0) {  // Check if Environment Sensor is working
          // ================================================== Internal Fan control by Temperature ================================================== //
          if (g_nEnvironmentTemperature >= g_pProfileSettings[g_nCurrentProfile].StartInternalFanTemperature) {  // If Environment Temperature if higher or equal to needed to Start Internal Fan Temperature, start it
            if (digitalRead(GetPinByName("Internal Fan"))) {
              digitalWrite(GetPinByName("Internal Fan"), RELAY_PIN_ON);

              LOGGER(INFO, "Internal Fan Started.");
            }
          } else if (g_nEnvironmentTemperature <= (g_pProfileSettings[g_nCurrentProfile].StartInternalFanTemperature - g_nTemperatureStopHysteresis)) {  // If Environment Temperature is less or equal to: Start Internal Fan Temperature - Temperature Hysteresis, stop it
            digitalWrite(GetPinByName("Internal Fan"), RELAY_PIN_OFF);

            LOGGER(INFO, "Internal Fan Stopped.");
          }
          // ================================================== Ventilation Fans control by Temperature & Humidity ================================================== //
          static bool bStartVentilationByTemperature = false;
          static bool bStartVentilationByHumidity = false;

          // Control by Temperature
          if (g_nEnvironmentTemperature >= g_pProfileSettings[g_nCurrentProfile].StartVentilationTemperature)
            bStartVentilationByTemperature = true;
          else if (g_nEnvironmentTemperature <= (g_pProfileSettings[g_nCurrentProfile].StartVentilationTemperature - g_nTemperatureStopHysteresis))
            bStartVentilationByTemperature = false;

          // Control by Humidity
          if (g_nEnvironmentHumidity >= g_pProfileSettings[g_nCurrentProfile].StartVentilationHumidity)
            bStartVentilationByHumidity = true;
          else if (g_nEnvironmentHumidity <= (g_pProfileSettings[g_nCurrentProfile].StartVentilationHumidity - g_nHumidityStopHysteresis))
            bStartVentilationByHumidity = false;
          ///////////////////////////////////////////////////
          if (bStartVentilationByTemperature || bStartVentilationByHumidity) {
            if (digitalRead(GetPinByName("Ventilation"))) {
              digitalWrite(GetPinByName("Ventilation"), RELAY_PIN_ON);

              LOGGER(INFO, "Ventilation Started.");
            }
          } else {
            digitalWrite(GetPinByName("Ventilation"), RELAY_PIN_OFF);

            LOGGER(INFO, "Ventilation Stopped.");
          }
        }
      }
    } else {  // If Fans are in Rest time
      if (g_nFansRestDuration > 0 && (nCurrentMillis - g_nFansRestElapsedTime) >= g_nFansRestDuration) {  // Check if g_nFansRestDuration just in case of SD is not connected at Controller startup
        g_bFansRest = false;
        g_nFansRestElapsedTime = 0;

        LOGGER(INFO, "Fans Rest time Completed.");
      }
    }
    // ================================================== Pumps Flow Test Section ================================================== //
    if (g_nTestPumpStartTime == 0 && (g_bTestIrrigationPump || g_bTestPHReducerPump || g_bTestVegetativeFertPump || g_bTestFloweringFertPump)) {
      const char* cPumpToTest = GetPumpToTest();

      if (cPumpToTest) {
        if (digitalRead(GetPinByName(cPumpToTest))) {
          digitalWrite(GetPinByName(cPumpToTest), RELAY_PIN_ON);

          g_nTestPumpStartTime = nCurrentMillis;

          LOGGER(INFO, "%s Flow test Started.", cPumpToTest);
        }
      }
    } else if (g_nTestPumpStartTime > 0 && (nCurrentMillis - g_nTestPumpStartTime) >= 60000) {  // NOTE: Dilemma, Actually, since it's within a 1-second check, it could stay on for up to 61 seconds. I'd have to analyze whether moving this outside the 1-second check would impact CPU usage
      const char* cPumpToTest = GetPumpToTest();

      if (cPumpToTest) {  // Just in case...
        digitalWrite(GetPinByName(cPumpToTest), RELAY_PIN_OFF);

        LOGGER(INFO, "%s Flow test Finished.", cPumpToTest);

        g_bTestIrrigationPump = false;
        g_bTestPHReducerPump = false;
        g_bTestVegetativeFertPump = false;
        g_bTestFloweringFertPump = false;
        g_nTestPumpStartTime = 0;
      }
    }
    // ================================================== Fertilizers Application Section ================================================== //
    if (g_bApplyFertilizers) {
      if (PowerSupplyControl(true)) {
        static uint32_t nFertilizersTimer = 0;

        if (nFertilizersTimer == 0) {
          nFertilizersTimer = nCurrentMillis;
        } else {
          static bool bWaitTime = false;

          if (!bWaitTime && (nCurrentMillis - nFertilizersTimer) >= 1500) { // Wait 1.5 seconds to stabilize the voltage output just in case
            bWaitTime = true;
          } else if (bWaitTime) {
            static uint8_t nStage = 0;

            switch (nStage) {
              case 0: // pH Reducer
                {
                  const char* cPump = "pH Reducer Pump";

                  if (digitalRead(GetPinByName(cPump))) {
                    digitalWrite(GetPinByName(cPump), RELAY_PIN_ON);

                    nFertilizersTimer = nCurrentMillis;

                    LOGGER(INFO, "%s Started.", cPump);
                  } else {
                    if ((nCurrentMillis - nFertilizersTimer) >= ((g_pProfileSettings[g_nCurrentProfile].PHReducerToApply * 60000) / g_nPHReducerFlowPerMinute)) {
                      digitalWrite(GetPinByName(cPump), RELAY_PIN_OFF);

                      LOGGER(INFO, "%s Stopped.", cPump);

                      nStage++;
                    }
                  }
                }
                break;
              case 1: // Vegetative Fertilizer
                {
                  const char* cPump = "Vegetative Fert Pump";

                  if (digitalRead(GetPinByName(cPump))) {
                    digitalWrite(GetPinByName(cPump), RELAY_PIN_ON);

                    nFertilizersTimer = nCurrentMillis;

                    LOGGER(INFO, "%s Started.", cPump);
                  } else {
                    if ((nCurrentMillis - nFertilizersTimer) >= ((g_pProfileSettings[g_nCurrentProfile].VegetativeFertilizerToApply * 60000) / g_nVegetativeFlowPerMinute)) {
                      digitalWrite(GetPinByName(cPump), RELAY_PIN_OFF);

                      LOGGER(INFO, "%s Stopped.", cPump);

                      nStage++;
                    }
                  }
                }
                break;
              case 2: // Flowering Fertilizer
                {
                  const char* cPump = "Flowering Fert Pump";

                  if (digitalRead(GetPinByName(cPump))) {
                    digitalWrite(GetPinByName(cPump), RELAY_PIN_ON);

                    nFertilizersTimer = nCurrentMillis;

                    LOGGER(INFO, "%s Started.", cPump);
                  } else {
                    if ((nCurrentMillis - nFertilizersTimer) >= ((g_pProfileSettings[g_nCurrentProfile].FloweringFertilizerToApply * 60000) / g_nFloweringFlowPerMinute)) {
                      digitalWrite(GetPinByName(cPump), RELAY_PIN_OFF);

                      LOGGER(INFO, "%s Stopped.", cPump);

                      PowerSupplyControl(false);

                      bWaitTime = false;
                      nStage = 0;
                      g_bApplyFertilizers = false;
                    }
                  }
                }
                break;
            }
          }
        }
      }
    }
    // ================================================== Store Data for Graph Section ================================================== //
    if ((nCurrentMillis - nLastStoreElapsedTime) >= g_nSamplingInterval) {
      nLastStoreElapsedTime = nCurrentMillis;

      String strValues = String(timeNow) + "|" + String(g_nEnvironmentTemperature) + "|" + String(g_nEnvironmentHumidity) + "|" + String(g_fEnvironmentVPD, 2);

      for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++) {
        g_nSoilsHumidity[i] = GetSoilHumidity(i);

        strValues += "|" + String(g_nSoilsHumidity[i]);
      }

      strValues += "|" + String((digitalRead(GetPinByName("Lights")) || (g_pProfileSettings[g_nCurrentProfile].LightBrightness < 401)) ? 0 // If digitalRead == true Or g_pProfileSettings[g_nCurrentProfile].LightBrightness is less than 401 equals to Relay OFF so: 0
                                                                                                                                       : g_pProfileSettings[g_nCurrentProfile].LightBrightness); // If not is OFF, return the Brightness Level

      WriteToSD("/metrics.log", strValues.c_str(), true);

      for (int8_t i = MAX_GRAPH_MARKS - 1; i > 0; i--) { // Shifts all values up one position
        strncpy(g_strArrayGraphData[i], g_strArrayGraphData[i - 1], MAX_GRAPH_MARKS_LENGTH - 1);
        g_strArrayGraphData[i][MAX_GRAPH_MARKS_LENGTH - 1] = '\0';
      }

      strncpy(g_strArrayGraphData[0], strValues.c_str(), MAX_GRAPH_MARKS_LENGTH - 1); // Store the current value at the beginning of the array
      g_strArrayGraphData[0][MAX_GRAPH_MARKS_LENGTH - 1] = '\0';
    }
  }
}
