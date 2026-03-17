//          _________________________________________________________________
//         /                                                                /\
//        /  _   __    _                   __                   ______     / /\
//       /  / | / /   (_)  _____  ____    / /  ____ _   _____  / ____/  __/ /
//      /  /  |/ /   / /  / ___/ / __ \  / /  / __ `/  / ___/ / / __   /\_\/
//     /  / /|  /   / /  / /__  / /_/ / / /  / /_/ /  (__  ) / /_/ /  /_/
//    /  /_/ |_/   /_/   \___/  \____/ /_/   \__,_/  /____/  \____/    /\
//   /                        Version 4 (2026)                        / /
//  /________________________________________________________________/ /
//  \________________________________________________________________\/
//   \    \    \    \    \    \    \    \    \    \    \    \    \    \

#define FIRMWAREVERSION "V420260317_141050" // TODO: Actualizar esto antes de compilar.

#include <map>
#include <Secrets.h>
#include <SD.h> // https://docs.arduino.cc/libraries/sd/#SD%20class
#include <WiFi.h>
#include <Update.h>
#include <SimpleDHT.h>  // DHT11: 5~95%RH ±5% | -20~60°C ±2°C (ASAIR Sensor) | DHT22: 0~100%RH ±2-5% | -40~80°C ±0.5°C (FMD)
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
// TODO: A futuro podría reescribir toda la lógica para poder funcionar con días de mas de 24 horas (trabajar con marcas de tiempo transcurrido en lugar de horas y días).
// TODO: A futuro sería ideal agregar un archivo durante el proceso de incorporación de Fertilizantes. Así en caso de pérdida de energía, se pueda reanudar el proceso donde se haya quedado. (En caso de hacerlo, poner esto es // NOTES: // After unexpected energy shutdown, the fertilizer incorporation process gonna continue with the remaining CC and/or remaining stages to be incorporated.)

// NOTES:
// Default IP for AP mode is: 192.168.4.1
// If Environment Humidity or Temperature Reads 0, the fans never gonna start.
// If Light Start & Stop Times Is 0, the light never gonna start.
// HW080 have a pulldown (in return line to gnd).
// All logics assume/is for Days of 24 Hours max.
// After unexpected energy shutdown, the irrigation process gonna re-do the last pulse if is needed, but it gonna do entire pulse, not just remaining time.
/* Fertilizer Incorporation Logic:
     The fertilizer incorporation process is controlled by the g_bApplyFertilizers flag.
     When triggered, the system inspects past irrigation days (g_vecWateringStages) to determine whether to apply fertilizers, based on the current incorporation mode (g_nFertilizerIncorporationMode).

Modes of operation:
  - Mode 0 (Permissive):
    If the current irrigation day (g_nIrrigationDayCounter) has TargetCC > 0,
    the system searches backward through previous days to find the most recent day
    containing at least one fertilizer value > 0.0cc.
    Fertilizer values from that day are then used for incorporation.

  - Mode 1 (Strict):
    Fertilizers are only incorporated if the current day has TargetCC > 0
    AND contains at least one fertilizer value > 0.0cc.
    No backward search is performed in this mode.

Once a valid fertilizer day is identified (based on mode):
  - The system prepares incorporation stages, one per fertilizer pump with a non-zero value.
  - Each stage calculates the pump duration based on the volume to apply and the configured flow rate.
  - The power supply is enabled, followed by a 1.5-second stabilization delay.
  - Fertilizer pumps are activated in sequence based on the scheduled durations.
  - After completion, the system shuts down the relays, resets internal state, and clears g_bApplyFertilizers.

Notes:
  - If no valid day with fertilizer > 0.0cc is found (per mode), no fertilizers are applied.
  - Minimum pump duration is enforced to be at least 1ms.
  - The incorporation process only runs once per trigger (based on nMaxStages == 0).*/

// Definitions
//#define TEST_MODE             // Use this to enable special web commands
//#define ENABLE_SERIAL_LOGGER  // Use this when debugging
#define ENABLE_SD_LOGGING     // Use this to save logs to SD Card
//#define ENABLE_AP_ALWAYS      // Use this to enable always the Access Point. Else it just enable when have no internet connection

#define FLOW_TEST_DURATION 10000 // 10 seconds

#define MAX_FERTILIZER_PUMPS 3  // pH Reducer, Vegetative & Flowering Fertilizers

#define USE_DHT22 // OR USE_DHT22

#define MAX_GRAPH_MARKS 48  // How much logs show in Web Panel Graph

#ifdef USE_DHT11
  #define MAX_GRAPH_MARKS_LENGTH 32 // How long text is (Example: 1749390362|100|100|100|100|4095) For each extra soil moisture sensor is 4 bytes more. Remember add a extra byte for null terminator
#elif defined(USE_DHT22)
  #define MAX_GRAPH_MARKS_LENGTH 36 // How long text is (Example: 1749390362|100.0|100.0|100|100|4095)
#endif

#define WIFI_RETRY_CONNECT_INTERVAL 60000 // 1 minute
#define WIFI_MAX_RETRYS 5 // Max Wifi reconnection attempts
#define WIFI_RETRY_INTERVAL 1000  // 1 second

#define WEBSERVER_PORT SECRET_WEBSERVER_PORT

#define ACCESSPOINT_NAME SECRET_ACCESSPOINT_NAME

#define TIMEZONE "ART3" // POSIX Format (To change website timezone, modify script.js)

#define CALLMEBOT_APY_KEY SECRET_CALLMEBOT_APY_KEY
#define CALLMEBOT_PHONE_TO_SEND SECRET_CALLMEBOT_PHONE_TO_SEND

#define TIME_SAVE_INTERVAL 10000  // 10 seconds

#define CHECK_RESERVOIR_LEVEL_INTERVAL 3600000  // 1 Hour

#define SECONDS_WAIT_AFTER_TURN_ON_POWER_SUPPLY 1500  // Wait 1.5 seconds to stabilize the

#define S8050_FREQUENCY 300   // https://www.mouser.com/datasheet/2/149/SS8050-117753.pdf
#define S8050_RESOLUTION 12
#define S8050_MAX_VALUE 4095  // Cuz is 12 bits of resolution

#define HW080_MIN 0         // https://cms.katranji.com/web/content/723081
#define HW080_MAX 2800      // I'm using 20k resistors, so the max value never go up to 4095
#define HW080_MAX_READS 10  // To get good soil humidity average

#define HCSR04_MAX_READS 10  // To get Irrigation Solution Level average

// Pins (Using an NodeMCU-32s v1.1)
#ifdef USE_DHT22
#define DHT_VCC_PIN 1
#endif

#define DHT_DATA_PIN 4  // I'm using 4.7k resistor between DATA & VCC

#define SD_CS_PIN 5 // Chip select for Enable/Disable SD Card

#define S8050_PWM_PIN 32  // I'm using a 1K resistor in serie in BASE Pin (Light Brightness controller) // https://www.mouser.com/datasheet/2/149/SS8050-117753.pdf

#define HW080_VCC_PIN 2  // I Enable this pin when want to read and disable it to prevent electrolysis

#define HCSR04_TRIGGER_PIN 14
#define HCSR04_ECHO_PIN 26

struct RelayPin {
  const char* Name; // Channel name
  uint8_t Pin;      // Pin number
};

struct SoilMoisturePin {
  uint8_t Pin;  // Pin Number
  const char HTMLColor[8];  // HTML Color for use in Graph on Web Panel
};

enum RELAYS_INDEX {
  LIGHTS,
  INTERNAL_FAN,
  VENTILATION_FANS,
  MIXING_PUMP,
  IRRIGATION_PUMP,
  FERTILIZER_PUMP_0,  // Used for pH Reducer
  FERTILIZER_PUMP_1,  // Used for Vegetative Fertilizer
  FERTILIZER_PUMP_2,  // Used for Flowering Fertilizer
  POWER_SUPPLY,       // Needed to turn ON: MIXING_PUMP to FERTILIZER_PUMP_2
  //WATER_ELECTROVALVE,
  RELAYS_INDEX_COUNT
};

const RelayPin RELAYS_MAP[RELAYS_INDEX_COUNT] = { // Add here more Pins from the Relays Module
  { "Lights", 16 },                     // Channel 0 of Relay Module 0  // NOTE: If the driver is turned off with a high value (Because the s8050 transistor is NPN) in S8050_PWM_PIN, I don't need to turn off the lights through the relay and use this channel for something else...
  { "Internal Fan", 17 },               // Channel 1 of Relay Module 0
  { "Ventilation Fans", 15 },           // Channel 2 of Relay Module 0
  { "Mixing Pump", 13 },                // Channel 3 of Relay Module 0
  { "Irrigation Pump", 21 },            // Channel 4 of Relay Module 0
  { "pH Reducer Pump", 22 },            // Channel 5 of Relay Module 0
  { "Vegetative Fertilizer Pump", 27 }, // Channel 6 of Relay Module 0
  { "Flowering Fertilizer Pump", 25 },  // Channel 7 of Relay Module 0

  { "Power Supply", 33 }                // Channel 0 of Relay Module 1
  //{ "Water Electrovalve", 3 }         // Channel 1 of Relay Module 1
};

const SoilMoisturePin pSoilMoisturePins[] = { // Add here more Pins for Soil Moisture
  { 34, "#B57165" },  // Soil Humidity Sensor 0
  { 35, "#784B43" }   // Soil Humidity Sensor 1
  /*{ 36, "#B54B65" },  // Soil Humidity Sensor 2
  { 39, "#788643" }   // Soil Humidity Sensor 3*/
};

// Global Constants
const uint8_t nSoilMoisturePinsCount = sizeof(pSoilMoisturePins) / sizeof(pSoilMoisturePins[0]);

// Global Variables
const char* g_strWebServerFiles[] = { // Add here files to be server by the webserver
  "/www/chart.js",
  "/www/fan.webp",
  "/www/style.css",
  "/www/script.js",
  "/www/logs.html"
};

// DO NOT TOUCH IT!
enum ERR_TYPE { INFO, WARN, ERROR };

#define RELAY_PIN_ON  LOW
#define RELAY_PIN_OFF HIGH

struct WateringData {
  uint16_t Day;
  uint16_t TargetCC;
  float FertilizerToApply[MAX_FERTILIZER_PUMPS] = { 0.0f };

  bool operator==(const WateringData& other) const {  // Little overload but needed for compare
    if (Day != other.Day || TargetCC != other.TargetCC)
      return false;

    for (uint8_t i = 0; i < MAX_FERTILIZER_PUMPS; ++i) {
      if (fabs(FertilizerToApply[i] - other.FertilizerToApply[i]) > 0.0001f)
        return false;
    }

    return true;
  }
};

struct FertilizerIncorporationStage {
  uint8_t Pin;  // Pin Number
  uint64_t Duration;  // Turn ON Time
};

const char* g_strProfiles[] = {
  "vegetative",
  "flowering",
  "drying"
};

// Settings Variables
char g_cSSID[32];
char g_cSSIDPWD[32];
uint32_t g_nCropBegin = 0;
uint64_t g_nSamplingInterval = 0;
uint64_t g_nFansRestInterval = 0;
uint64_t g_nFansRestDuration = 0;
uint16_t g_nTemperatureStopHysteresis = 0;
uint16_t g_nHumidityStopHysteresis = 0;
uint16_t g_nIrrigationFlowPerMinute = 0;
uint16_t g_nFertilizersPumpsFlowPerMinute[MAX_FERTILIZER_PUMPS] = { 0 };
uint64_t g_nMixingPumpDuration = 0;
uint16_t g_nIrrigationReservoirLowerLevel = 0;
uint8_t g_nStartLightTime = 0;
uint8_t g_nStopLightTime = 0;
uint16_t g_nLightBrightness = 0;
uint8_t g_nInternalFanMode = 0;
uint8_t g_nStartInternalFanTemperature = 0;
uint8_t g_nVentilationMode = 0;
uint8_t g_nStartVentilationTemperature = 0;
uint8_t g_nStartVentilationHumidity = 0;
uint16_t g_nIrrigationDayCounter = 1;
int8_t g_nLastWateredHour = -1;
uint8_t g_nFertilizerIncorporationMode = 0;
std::vector<WateringData> g_vecWateringStages;
///////////////////////////////////////////
static std::map<String, File> g_mapUploadFiles;
bool g_bIsSDInit = false;
uint8_t g_nCurrentProfile = 0;
uint8_t g_nEffectiveStartLights = 0;
uint8_t g_nEffectiveStopLights = 0;
uint64_t g_nIrrigationDuration = 0;

#ifdef USE_DHT11
  uint8_t g_nEnvironmentTemperature = 0;
  uint8_t g_nEnvironmentHumidity = 0;
#elif defined(USE_DHT22)
  float g_fEnvironmentTemperature = 0.0f;
  float g_fEnvironmentHumidity = 0.0f;
#endif

uint8_t g_nIrrigationSolutionLevel = 0;
uint8_t g_nSoilsHumidity[nSoilMoisturePinsCount] = { 0 };
uint64_t g_nFansRestTimeStartedAt = 0;
uint8_t g_nTestPumpID = 0;  // 0 Equals to no pump to test
bool g_bApplyFertilizers = false;
bool g_bManualMixing = false;
char g_strArrayGraphData[MAX_GRAPH_MARKS][MAX_GRAPH_MARKS_LENGTH] = {};

// Global Handles, Interface & Instances
AsyncWebServer g_pWebServer(WEBSERVER_PORT);  // Asynchronous web server instance listening on WEBSERVER_PORT

#ifdef USE_DHT11
  SimpleDHT11 g_pDHT11(DHT_DATA_PIN);         // Interface to DHT11 Temperature & Humidity sensor
#elif defined(USE_DHT22)
  SimpleDHT22 g_pDHT22(DHT_DATA_PIN);         // Interface to DHT22 Temperature & Humidity sensor
#endif

TaskHandle_t g_pWiFiReconnect;                // Task handle for Wifi reconnect logic running on core 0
SemaphoreHandle_t g_pSDMutex;                 // Mutex to synchronize concurrent access to the SD card across tasks
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t millis64() { return esp_timer_get_time() / 1000ULL; }
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sets the system time and timezone based on a given Unix timestamp.
// Updates the system's internal clock to the provided timestamp (seconds since epoch).
void SetCurrentDatetime(time_t unixTimestamp) {
  struct timeval tv;
  tv.tv_sec = unixTimestamp;
  tv.tv_usec = 0;

  settimeofday(&tv, nullptr);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Retrieves the current local time and stores it in the provided tm struct.
// Uses the system time (UTC) and converts it to local time based on the configured timezone.
// The result is stored in the pTimeInfo pointer passed by the caller.
// Returns true if the conversion was successful, false otherwise.
bool GetLocalTimeNow(struct tm* pTimeInfo) {
  time_t pTimeNow = time(nullptr);
  return localtime_r(&pTimeNow, pTimeInfo) != nullptr;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads a line from the given file stream into the provided buffer.
// Ensures the buffer is null-terminated and trims trailing whitespace (e.g., spaces, tabs, newlines).
// - pFile: reference to the open File object.
// - pBuffer: pointer to the destination character buffer (must be large enough to hold the line).
// - nBufferSize: size of the destination buffer (including null terminator).
void ReadFromStream(File& pFile, char* pBuffer, size_t nBufferSize) {
  size_t sizeLength = pFile.readBytesUntil('\n', pBuffer, nBufferSize - 1);

  pBuffer[sizeLength] = '\0';

  while (sizeLength > 0 && isspace(pBuffer[sizeLength - 1]))
    pBuffer[--sizeLength] = '\0';
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Provides utility functions to convert between ticks (milliseconds) and human-readable time units.
// Ticks are assumed to be in milliseconds, as returned by the millis() function.
inline uint32_t TicksToSeconds(uint32_t nTicks) { return nTicks / 1000; }
inline uint32_t TicksToMinutes(uint32_t nTicks) { return nTicks / (1000 * 60); }
//inline uint32_t TicksToHours(uint32_t nTicks) { return nTicks / (1000 * 60 * 60); }

inline uint32_t SecondsToTicks(uint32_t nSeconds) { return nSeconds * 1000; }
inline uint32_t MinutesToTicks(uint32_t nMinutes) { return nMinutes * 1000 * 60; }
//inline uint32_t HoursToTicks(uint32_t nHours) { return nHours * 1000 * 60 * 60; }
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Executes the provided function (`fn`) with safe, exclusive access to the SD card.
// - Tries to acquire the SD card mutex within 100 ms to ensure thread-safe access.
//   If the mutex cannot be acquired, prints a warning via Serial (only in TEST_MODE & ENABLE_SERIAL_LOGGER) and exits.
// - Once the mutex is acquired, it is automatically released when the function scope ends,
//   using RAII via the ScopedMutexUnlock helper.
// - If the SD card is not yet initialized (`g_bIsSDInit` is false), attempts initialization via SD.begin().
//   If initialization fails, logs the failure (only in TEST_MODE & ENABLE_SERIAL_LOGGER), and returns.
// - After initialization, checks whether a valid SD card is inserted (cardType != CARD_NONE).
//   If not present, logs an error (TEST_MODE & ENABLE_SERIAL_LOGGER only), resets internal SD state, calls SD.end(), and exits.
// - Verifies filesystem access by attempting to open the root directory.
//   If the directory is invalid or inaccessible, logs an error (TEST_MODE & ENABLE_SERIAL_LOGGER only),
//   resets internal SD state, calls SD.end(), and exits.
// - If all checks pass, the user-provided function `fn()` is executed while the mutex is held.
void SafeSDAccess(std::function<void()> fn) {
  if (!xSemaphoreTake(g_pSDMutex, pdMS_TO_TICKS(100 /*ms*/))) {
#if defined(TEST_MODE) && defined(ENABLE_SERIAL_LOGGER)
    Serial.println("[WARN] SD Mutex TimeOut.");
#endif

    return;
  }

  struct ScopedMutexUnlock {
    SemaphoreHandle_t& mutex;
    ~ScopedMutexUnlock() { xSemaphoreGive(mutex); }
  } unlocker{g_pSDMutex};

  if (!g_bIsSDInit) {
    g_bIsSDInit = SD.begin(SD_CS_PIN);

    if (!g_bIsSDInit) {
#if defined(TEST_MODE) && defined(ENABLE_SERIAL_LOGGER)
      Serial.println("[ERROR] Failed to initialize SD.");
#endif

      return;
    }
  }

  if (SD.cardType() == CARD_NONE) {
#if defined(TEST_MODE) && defined(ENABLE_SERIAL_LOGGER)
    Serial.println("[ERROR] No SD card detected.");
#endif

    g_bIsSDInit = false;

    SD.end();

    return;
  }

  File pRoot = SD.open("/");
  if (!pRoot || !pRoot.isDirectory()) {
#if defined(TEST_MODE) && defined(ENABLE_SERIAL_LOGGER)
    Serial.println("[ERROR] Filesystem not accessible or corrupted.");
#endif

    g_bIsSDInit = false;

    SD.end();

    return;
  } else {
    pRoot.close();
  }

  fn();
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Writes a line of text to a file on the SD card.
// - strFileName: Path of the file to write to.
// - strText: Text string to be written.
// - bAppend: If true, appends to the file; otherwise, overwrites it.
// - bUseSafeSDAccess: If true, wraps the operation inside SafeSDAccess for thread-safe access.
// If the SD is not initialized, the function exits without performing any action.
void WriteToSD(const char* strFileName, const char* strText, bool bAppend, bool bUseSafeSDAccess = true) {
  auto Write = [&]() {
    if (!g_bIsSDInit)
      return;

    char cFinalFileName[64];

    strncpy(cFinalFileName, strFileName, sizeof(cFinalFileName));
    cFinalFileName[sizeof(cFinalFileName) - 1] = '\0';

    File pFile = SD.open(cFinalFileName, bAppend ? FILE_APPEND : FILE_WRITE);
    if (pFile) {
      pFile.println(strText);

      pFile.close();
    }
  };

  if (bUseSafeSDAccess)
    SafeSDAccess(Write);
  else
    Write();
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Logs a formatted message with severity and timestamp.
// Parameters:
// - nType: Log severity (INFO, WARN, ERROR).
// - bUseSafeSDAccess: Indicates whether SD access should be protected.
// - strFormat: printf-style format string with optional arguments.
// Behavior:
// - Prepends timestamp (DD/MM/YYYY HH:MM:SS) and severity tag.
// - Outputs to a daily SD log file if ENABLE_SD_LOGGING is defined.
// - Prints to Serial if ENABLE_SERIAL_LOGGER is defined.
void LOGGER(ERR_TYPE nType, bool bUseSafeSDAccess, const char* strFormat, ...) {
#if defined(ENABLE_SD_LOGGING) || defined(ENABLE_SERIAL_LOGGER)
  va_list args;

  char cTimestamp[20], cPrintType[9], cBuffer[384];
  uint8_t nOffset = 0;

  struct tm currentTime;
  GetLocalTimeNow(&currentTime);
  snprintf(cTimestamp, sizeof(cTimestamp), "%02d/%02d/%04d %02d:%02d:%02d ", currentTime.tm_mday, currentTime.tm_mon + 1, currentTime.tm_year + 1900, currentTime.tm_hour, currentTime.tm_min, currentTime.tm_sec);

  switch (nType) {
    case INFO:  snprintf(cPrintType, sizeof(cPrintType), "[INFO] "); break;
    case WARN:  snprintf(cPrintType, sizeof(cPrintType), "[WARN] "); break;
    case ERROR: snprintf(cPrintType, sizeof(cPrintType), "[ERROR] "); break;
  }

  nOffset = snprintf(cBuffer, sizeof(cBuffer), "%s %s", cTimestamp, cPrintType);

  va_start(args, strFormat);

  vsnprintf(cBuffer + nOffset, sizeof(cBuffer) - nOffset, strFormat, args);

  va_end(args);

#ifdef ENABLE_SD_LOGGING
  char cFinalFileName[29];

  snprintf(cFinalFileName, sizeof(cFinalFileName), "/logs/logging_%02d_%02d_%04d.txt", currentTime.tm_mday, currentTime.tm_mon + 1, currentTime.tm_year + 1900);

  WriteToSD(cFinalFileName, cBuffer, true, bUseSafeSDAccess);
#endif
#ifdef ENABLE_SERIAL_LOGGER
  Serial.println(cBuffer);
#endif
#endif
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

      pSettingsFile.println(g_nCropBegin);

      pSettingsFile.println(g_nSamplingInterval);

      pSettingsFile.println(g_nCurrentProfile);

      pSettingsFile.println(g_nFansRestInterval);
      pSettingsFile.println(g_nFansRestDuration);

      pSettingsFile.println(g_nTemperatureStopHysteresis);
      pSettingsFile.println(g_nHumidityStopHysteresis);

      pSettingsFile.println(g_nIrrigationFlowPerMinute);

      for (uint8_t i = 0; i < MAX_FERTILIZER_PUMPS; i++)
        pSettingsFile.println(g_nFertilizersPumpsFlowPerMinute[i]);

      pSettingsFile.println(g_nMixingPumpDuration);

      pSettingsFile.println(g_nIrrigationReservoirLowerLevel);

      pSettingsFile.close();

      LOGGER(INFO, false, "Settings file updated successfully.");
    }
  });
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Loads a profile from the SD card.
void LoadProfile(uint8_t nProfile) {
  SafeSDAccess([&]() {
    if (!g_bIsSDInit)
      return;

    char cBuffer[64];

    File pProfileFile = SD.open(String("/profiles/") + g_strProfiles[nProfile], FILE_READ);
    if (pProfileFile) {
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // START LIGHT TIME
      g_nStartLightTime = atoi(cBuffer);

      g_nEffectiveStartLights = (g_nStartLightTime == 24) ? 0 : g_nStartLightTime;  // Stores the effective light start hour, converting hour 24 to 0 (midnight)
      ///////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // STOP LIGHT TIME
      g_nStopLightTime = atoi(cBuffer);

      g_nEffectiveStopLights = (g_nStopLightTime == 24) ? 0 : g_nStopLightTime; // Stores the effective light stop hour, converting hour 24 to 0 (midnight)
      ///////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // LIGHT BRIGHTNESS LEVEL
      g_nLightBrightness = atoi(cBuffer);
      ///////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // INTERNAL FAN MODE
      g_nInternalFanMode = atoi(cBuffer);
      ///////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // INTERNAL FAN TEMPERATURE START
      g_nStartInternalFanTemperature = atoi(cBuffer);
      //////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // VENTILATION FANS MODE
      g_nVentilationMode = atoi(cBuffer);
      ///////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // VENTILATION FANS TEMPERATURE START
      g_nStartVentilationTemperature = atoi(cBuffer);
      ///////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // VENTILATION FANS HUMIDITY START
      g_nStartVentilationHumidity = atoi(cBuffer);
      ///////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // IRRIGATION DAYS COUNTER
      g_nIrrigationDayCounter = atoi(cBuffer);
      ///////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // LAST IRRIGATION EXECUTE HOUR
      g_nLastWateredHour = atoi(cBuffer);
      ///////////////////////////////////////////////////
      ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // FERTILIZERS INCORPORATION MODE
      g_nFertilizerIncorporationMode = atoi(cBuffer);
      ///////////////////////////////////////////////////     READ IRRIGATION SCHEME DATA
      g_vecWateringStages.clear();

      while (pProfileFile.available()) {
        ReadFromStream(pProfileFile, cBuffer, sizeof(cBuffer)); // IRRIGATION DAY|IRRIGATION CC|FERTILIZER A CC|FERTILIZER B CC|ETC

        WateringData pData = {};

        char* cToken = strtok(cBuffer, "|");
        pData.Day = atoi(cToken);

        cToken = strtok(nullptr, "|");
        pData.TargetCC = atoi(cToken);

        for (uint8_t i = 0; i < MAX_FERTILIZER_PUMPS; ++i) {
          cToken = strtok(nullptr, "|");

          pData.FertilizerToApply[i] = atof(cToken);
        }

        g_vecWateringStages.push_back(pData);
      }

      pProfileFile.close();

      LOGGER(INFO, false, "Profile: %s loaded successfully.", g_strProfiles[nProfile]);
    }
  });
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Saves the current configuration to the SD card.
void SaveProfile(uint8_t nProfile) {
  SafeSDAccess([&]() {
    if (!g_bIsSDInit)
      return;

    File pProfileFile = SD.open(String("/profiles/") + g_strProfiles[nProfile], FILE_WRITE);
    if (pProfileFile) {
      pProfileFile.println(g_nStartLightTime);
      pProfileFile.println(g_nStopLightTime);
      pProfileFile.println(g_nLightBrightness);

      pProfileFile.println(g_nInternalFanMode);
      pProfileFile.println(g_nStartInternalFanTemperature);

      pProfileFile.println(g_nVentilationMode);
      pProfileFile.println(g_nStartVentilationTemperature);
      pProfileFile.println(g_nStartVentilationHumidity);

      pProfileFile.println(g_nIrrigationDayCounter);
      pProfileFile.println(g_nLastWateredHour);

      pProfileFile.println(g_nFertilizerIncorporationMode);

      /////////////////////////////////////////////////// SAVE IRRIGATION SCHEME DATA
      for (const auto& Watering : g_vecWateringStages) {
        pProfileFile.printf("%u|%u", Watering.Day, Watering.TargetCC);

        for (uint8_t i = 0; i < MAX_FERTILIZER_PUMPS; ++i)
          pProfileFile.printf("|%.1f", Watering.FertilizerToApply[i]);

        pProfileFile.println();
      }

      pProfileFile.close();

      LOGGER(INFO, false, "Profile: %s updated successfully.", g_strProfiles[nProfile]);
    }
  });
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Controls the relay channel associated with the Power Supply by setting its state.
// If bState is true, attempts to turn the relay ON (only if it was previously OFF).
// If bState is false, attempts to turn the relay OFF, but only if no dependent relays are currently active.
// Dependent relays include: Mixing Pump, Irrigation Pump, Pump0, Pump1, and Pump2.
// Returns true if the state change was successful or already in the desired state; false if blocked or failed.
bool PowerSupplyControl(bool bState) {
  int8_t nState = -1;

  if (bState) {
    if (digitalRead(RELAYS_MAP[POWER_SUPPLY].Pin))
      nState = RELAY_PIN_ON;
    else
      return true;
  } else {
    for (uint8_t i = MIXING_PUMP; i <= FERTILIZER_PUMP_2; i++) { // WARNING: Hardcode check
      if (!digitalRead(RELAYS_MAP[i].Pin)) {
        LOGGER(INFO, true, "The Power Supply cannot turn OFF because is used by other Process.");

        return false;
      }
    }

    if (!digitalRead(RELAYS_MAP[POWER_SUPPLY].Pin))
      nState = RELAY_PIN_OFF;
    else
      return true;
  }

  if (nState != -1) {
    digitalWrite(RELAYS_MAP[POWER_SUPPLY].Pin, nState);

    LOGGER(INFO, true, "The Power Supply was turned %s.", bState ? "ON" : "OFF");

    return true;
  }

  LOGGER(ERROR, true, "Was not possible to turn %s The Power Supply.", bState ? "ON" : "OFF");

  return false;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sends a WhatsApp notification message using the CallMeBot API.
// - strMessage: Text message to send (will be URL-encoded automatically)
// Returns early if WiFi is not connected.
// Encodes special characters using percent-encoding (%XX format).
// Characters allowed without encoding: alphanumeric, dash, underscore, dot, tilde.
// Message is truncated silently if encoded length exceeds 256 bytes.
// Logs an error if the HTTP request fails (non-200 response code).
void SendNotification(const char* strMessage) {
  if (WiFi.status() != WL_CONNECTED) {
    LOGGER(ERROR, true, "Cannot send notification, WiFi not connected.");
    return;
  }

  size_t i, j = 0;
  char cEncodedMessage[256] = { 0 };

  for (i = 0; strMessage[i] != '\0'; ++i) {
    char c = strMessage[i];

    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      if (j < sizeof(cEncodedMessage) - 1) {
        cEncodedMessage[j++] = c;
      } else {
        break;
      }
    } else {
      if (j < sizeof(cEncodedMessage) - 3) {
        snprintf(&cEncodedMessage[j], 4, "%%%02X", (unsigned char)c);

        j += 3;
      } else {
        break;
      }
    }
  }

  cEncodedMessage[j] = '\0';

  char cFinalUrl[256];
  snprintf(cFinalUrl, sizeof(cFinalUrl), "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s", CALLMEBOT_PHONE_TO_SEND, cEncodedMessage, CALLMEBOT_APY_KEY);

  HTTPClient http;
  http.begin(cFinalUrl);
  http.setTimeout(3000);

  int16_t nReturnCode = http.GET();
  if (nReturnCode != 200)
    LOGGER(ERROR, true, "Error sending notification. HTTP Code: %d", nReturnCode);

  http.end();
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Measures the irrigation reservoir level using an HC-SR04 ultrasonic sensor.
// Performs multiple distance readings to improve accuracy, discarding the highest and lowest values
// to filter outliers, then averaging the remaining valid results.
// Each reading triggers the sensor and waits for the echo, discarding timeouts.
// Returns the average distance (in centimeters) as an integer (int16_t) from the sensor to the water surface.
// If all readings fail or there are not enough valid readings, logs an error and returns -1.
int16_t GetIrrigationReservoirLevel() {
  float fReadings[HCSR04_MAX_READS];
  uint8_t nValidReads = 0;

  for (uint8_t i = 0; i < HCSR04_MAX_READS; i++) {
    digitalWrite(HCSR04_TRIGGER_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(HCSR04_TRIGGER_PIN, HIGH);
    delayMicroseconds(10);  // Small Wait to obtain an stable reading
    digitalWrite(HCSR04_TRIGGER_PIN, LOW);

    unsigned long lDuration = pulseIn(HCSR04_ECHO_PIN, HIGH, 23530);  // ~4 meters

    if (lDuration == 0)
      LOGGER(ERROR, true, "Irrigation Solution Level read out of range.");
    else
      fReadings[nValidReads++] = (lDuration * 0.0343f) / 2.0f;

    if (HCSR04_MAX_READS > 1 && i < (HCSR04_MAX_READS - 1))
      delay(60); // Small delay between reads
  }

  if (nValidReads == 0) {
    LOGGER(ERROR, true, "All HCSR04 readings failed.");

    return -1;
  }

  if (nValidReads <= 2) {
    LOGGER(ERROR, true, "Not enough HCSR04 readings.");

    return static_cast<int16_t>(fReadings[0]);
  }

  // Find the largest and smallest
  float fMin = fReadings[0], fMax = fReadings[0];

  for (uint8_t i = 1; i < nValidReads; i++) {
    if (fReadings[i] < fMin)
      fMin = fReadings[i];

    if (fReadings[i] > fMax)
      fMax = fReadings[i];
  }

  // Average without higher or lower limits
  float fSum = 0;
  bool bMinRemoved = false, bMaxRemoved = false;

  for (uint8_t i = 0; i < nValidReads; i++) {
    if (!bMinRemoved && fReadings[i] == fMin) {
      bMinRemoved = true;

      continue;
    }

    if (!bMaxRemoved && fReadings[i] == fMax) {
      bMaxRemoved = true;

      continue;
    }

    fSum += fReadings[i];
  }

  return static_cast<int16_t>(fSum / (nValidReads - 2));
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads the average humidity value from the specified soil sensor index.
// Excites the sensor by setting the VCC pin high and waits for stable readings.
// Takes multiple analog readings, calculates the average, and maps it to a percentage scale (0~100).
// After readings, the VCC pin is set low to prevent electrolysis.
// Returns the normalized humidity value as a uint8_t between 0 and 100.
uint8_t GetSoilHumidity(uint8_t nSensorNumber) {
  if (!digitalRead(HW080_VCC_PIN))
    digitalWrite(HW080_VCC_PIN, HIGH);  // Put Pin output in High to excite the moisture sensors

  delay(60);  // Small Wait to obtain an stable reading

  uint32_t nCombinedValues = 0;

  for (uint8_t i = 0; i < HW080_MAX_READS; i++) {
    nCombinedValues += analogRead(pSoilMoisturePins[nSensorNumber].Pin);

    if (HW080_MAX_READS > 1 && i < (HW080_MAX_READS - 1))
      delay(60); // Small delay between reads
  }

  digitalWrite(HW080_VCC_PIN, LOW); // Put pin output in Low to prevent electrolysis

  return constrain(map(nCombinedValues / HW080_MAX_READS, HW080_MIN, HW080_MAX, 0, 100), 0, 100);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads the irrigation reservoir sensor and updates the global reservoir level percentage.
// The level is calculated relative to the configured lower reference level and clamped to 0–100%.
// The update occurs only if the reference level is valid (>0) and the sensor reading is valid (-1 indicates an error).
void CheckReservoirLevel() {
  if (g_nIrrigationReservoirLowerLevel > 0) {
    int16_t nLowerLevel = GetIrrigationReservoirLevel();

    if (nLowerLevel != -1) 
      g_nIrrigationSolutionLevel = std::clamp(static_cast<uint8_t>(100.0f * (g_nIrrigationReservoirLowerLevel - nLowerLevel) / g_nIrrigationReservoirLowerLevel), static_cast<uint8_t>(0), static_cast<uint8_t>(100));
  }
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads all configured soil moisture sensors and updates the global humidity array.
// Each sensor is queried sequentially using its index, and the measured value is stored
// in the corresponding position of g_nSoilsHumidity.
void CheckSoilHumidity() {
  for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++)
    g_nSoilsHumidity[i] = GetSoilHumidity(i);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Checks for the presence of a specified argument in the web request and updates a destination variable.
// Compares the provided value from the request with the current one. If they differ, the destination is updated.
// Appends a feedback message to the return string if an update occurs, using a flash-resident message to save RAM.
// Supports generic numeric types via templating (e.g., uint8_t, uint16_t, int).
// Returns true if the value was changed, false otherwise.
template <typename T>
bool CheckNSetValue(AsyncWebServerRequest* pRequest, const char* strArgumentName, T& Destination, const __FlashStringHelper* flashMessageToReturn, String& strReturn) {
  if (pRequest->hasArg(strArgumentName)) {
    T NewValue = static_cast<T>(pRequest->arg(strArgumentName).toInt());

    if (NewValue != Destination) {
      Destination = NewValue;
      strReturn += F("Se actualizó ");
      strReturn += flashMessageToReturn;
      strReturn += F(".\r\n");

      return true;
    }
  }

  return false;
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
      LOGGER(INFO, true, "Starting Access Point (SSID: %s) mode for reconfiguration...", ACCESSPOINT_NAME);

      WiFi.mode(WIFI_AP_STA); // Set dual mode, Access Point & Station

      vTaskDelay(100 / portTICK_PERIOD_MS); // Delay to stabilize AP

      WiFi.softAP(ACCESSPOINT_NAME); // Start Access Point, while try to connect to Wifi
    }
#endif

    WiFi.disconnect(true);

    LOGGER(INFO, true, "Trying to reconnect Wifi...");

    WiFi.begin(g_cSSID, g_cSSIDPWD);

    uint8_t nConnectTrysCount = 0;

    while (nConnectTrysCount < WIFI_MAX_RETRYS && WiFi.status() != WL_CONNECTED) {
      nConnectTrysCount++;

      vTaskDelay(WIFI_RETRY_INTERVAL / portTICK_PERIOD_MS);  // Wait before trying again
    }

    if (WiFi.status() == WL_CONNECTED) {
      LOGGER(INFO, true, "Connected to Wifi SSID: %s PASSWORD: %s. IP: %s.", g_cSSID, g_cSSIDPWD, WiFi.localIP().toString().c_str());

#if !defined(ENABLE_AP_ALWAYS)
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);

      LOGGER(INFO, true, "Access Point disconnected.");
#endif
    } else {
      LOGGER(ERROR, true, "Max Wifi reconnect attempts reached.");
    }

    vTaskSuspend(NULL); // Suspends the task until needed again
  }
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Generates and returns dynamic HTML content or variable values based on the input key `var`. Just for the first web request.
// Supports returning environmental data, system states, and dynamically generated HTML blocks
// for insertion into templates. Used with templating engines (e.g., AsyncWebServer's .setTemplateProcessor).
String HTMLProcessor(const String& var) {
  if (var == "MAXBRIGHT") {
    return String(S8050_MAX_VALUE);
  } else if (var == "ELEMENTVALUES") {
    String strReturn = String(g_nStartLightTime) + "," + String(g_nStopLightTime);
    strReturn += "," + String(g_nIrrigationDayCounter);
    strReturn += "," + String(g_nStartInternalFanTemperature) + "," + String(g_nStartVentilationTemperature) + "," + String(g_nStartVentilationHumidity);
    strReturn += "," + String(g_nTemperatureStopHysteresis) + "," + String(g_nHumidityStopHysteresis);
    strReturn += "," + String(TicksToMinutes(g_nFansRestInterval)) + "," + String(TicksToMinutes(g_nFansRestDuration));
    strReturn += "," + String(g_nIrrigationFlowPerMinute);

    for (uint8_t i = 0; i < MAX_FERTILIZER_PUMPS; i++)
      strReturn += "," + String(g_nFertilizersPumpsFlowPerMinute[i]);

    strReturn += "," + String(TicksToSeconds(g_nMixingPumpDuration)) + "," + String(TicksToMinutes(g_nSamplingInterval));

    return strReturn;
  } else if (var == "SOILLABELS") {
    String strReturn;

    for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++)
      strReturn += "{label:'Humedad de Maceta " + String(i) + "',borderColor:'" + pSoilMoisturePins[i].HTMLColor + "',backgroundColor:'" + pSoilMoisturePins[i].HTMLColor + "',symbol:'%%'},";

    return strReturn;
  } else if (var == "PROFILE") {
    return String(g_nCurrentProfile);
  } else if (var == "BRIGHTLEVEL") {
    return String((g_nLightBrightness < 401) ? 0 : g_nLightBrightness); // WARNING: Hardcode offset
  } else if (var == "SOILINDICATORS") {
    String strReturn;

    for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++)
      strReturn += "<ind id=ind_soil" + String(i) + "></ind>";

    return strReturn;
  } else if (var == "CROPBEGIN") {
    struct tm currentTime;
    time_t t = (time_t)g_nCropBegin;

    localtime_r(&t, &currentTime);

    char cBuffer[11];
    snprintf(cBuffer, sizeof(cBuffer), "%02d/%02d/%04d", currentTime.tm_mday, currentTime.tm_mon + 1, currentTime.tm_year + 1900);
    return String(cBuffer);
  } else if (var == "CROPDAYCOUNTER") {
    uint32_t ElapsedCropDays = 0;

    time_t pTimeNow = time(nullptr);
    if (g_nCropBegin > 0 && pTimeNow >= g_nCropBegin)
      ElapsedCropDays = (pTimeNow - g_nCropBegin) / 86400;

    return String(ElapsedCropDays);
  } else if (var == "INTERNALFANMODE") {
    return String(g_nInternalFanMode);
  } else if (var == "RECIRCULATIONFANSMODE") {
    return String(g_nVentilationMode);
  } else if (var == "FERTILIZERSINCORPORATIONMODE") {
    return String(g_nFertilizerIncorporationMode);
  } else if (var == "FLOWTESTDURATION") {
    return String (TicksToSeconds(FLOW_TEST_DURATION));
  } else if (var == "SSID") {
    return String(g_cSSID);
  } else if (var == "SSIDPWD") {
    return String(g_cSSIDPWD);
  } else if (var == "FIRMWAREVERSION") {
    return FIRMWAREVERSION;
  }

  return String();
}

void setup() {
#ifdef ENABLE_SERIAL_LOGGER
  Serial.begin(115200);
  delay(3000);  // Small delay cuz this trash don't print the initial log
#endif

  g_pSDMutex = xSemaphoreCreateMutex();

  LOGGER(INFO, true, "========== Indoor Controller Started ==========");
  LOGGER(INFO, true, "Firmware Version: %s", FIRMWAREVERSION);
  LOGGER(INFO, true, "Initializing Pins...");

  for (uint8_t i = 0; i < RELAYS_INDEX_COUNT; i++) {
    pinMode(RELAYS_MAP[i].Pin, OUTPUT); // Set Pin Mode
    digitalWrite(RELAYS_MAP[i].Pin, RELAY_PIN_OFF); // Set default Pin State

    LOGGER(INFO, true, "%s Pin Done!", RELAYS_MAP[i].Name);
  }

#ifdef USE_DHT22
  pinMode(DHT_VCC_PIN, OUTPUT);
  digitalWrite(DHT_VCC_PIN, HIGH);
  LOGGER(INFO, true, "DHT VCC Pin Done!");
#endif

  ledcAttach(S8050_PWM_PIN, S8050_FREQUENCY, S8050_RESOLUTION);
  LOGGER(INFO, true, "Light Brightness Pin Done!");

  pinMode(HW080_VCC_PIN, OUTPUT);
  digitalWrite(HW080_VCC_PIN, LOW);
  LOGGER(INFO, true, "Power Pin for Soil Humidity Sensors Done!");

  for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++) {
    pinMode(pSoilMoisturePins[i].Pin, INPUT);

    LOGGER(INFO, true, "Soil Humidity Pin %d Done!", i);
  }

  pinMode(HCSR04_TRIGGER_PIN, OUTPUT);
  digitalWrite(HCSR04_TRIGGER_PIN, LOW);
  pinMode(HCSR04_ECHO_PIN, INPUT);
  LOGGER(INFO, true, "Pins for Irrigation Solution Reservoir Level Done!");

  LOGGER(INFO, true, "Loading Settings & Time...");

  SafeSDAccess([&]() {
    if (g_bIsSDInit) {
      File pSettingsFile = SD.open("/settings", FILE_READ); // Read Settings File
      if (pSettingsFile) {
        char cBuffer[64];
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // SSID

        strncpy(g_cSSID, cBuffer, sizeof(g_cSSID));
        g_cSSID[sizeof(g_cSSID) - 1] = '\0';
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // SSID PASSWORD

        strncpy(g_cSSIDPWD, cBuffer, sizeof(g_cSSIDPWD));
        g_cSSIDPWD[sizeof(g_cSSIDPWD) - 1] = '\0';
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // UNIXTIMESTAMP OF CROP BEGIN
        g_nCropBegin = atoi(cBuffer);
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // SAMPLING TAKE INTERVALS FOR GRAPH
        g_nSamplingInterval = atoi(cBuffer);
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // CURRENT PROFILE
        g_nCurrentProfile = atoi(cBuffer);
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // FANS REST INTERVAL
        g_nFansRestInterval = atoi(cBuffer);
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // FANS REST DURATION
        g_nFansRestDuration = atoi(cBuffer);
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // TEMPERATURE HYSTERESIS TO STOP FANS
        g_nTemperatureStopHysteresis = atoi(cBuffer);
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // HUMIDITY HYSTERESIS TO STOP FANS
        g_nHumidityStopHysteresis = atoi(cBuffer);
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // IRRIGATION PUMP FLOW PER MINUTE
        g_nIrrigationFlowPerMinute = atoi(cBuffer);
        ///////////////////////////////////////////////////
        for (uint8_t i = 0; i < MAX_FERTILIZER_PUMPS; i++) {      // CC FLOW PER MINUTE OF EACH PUMPS
          ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));
          g_nFertilizersPumpsFlowPerMinute[i] = atoi(cBuffer);
        }
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // MIXING PUMP DURATION
        g_nMixingPumpDuration = atoi(cBuffer);
        ///////////////////////////////////////////////////
        ReadFromStream(pSettingsFile, cBuffer, sizeof(cBuffer));  // LOWER POINT OF IRRIGATION SOLUTION RESERVOIR
        g_nIrrigationReservoirLowerLevel = atoi(cBuffer);
        ///////////////////////////////////////////////////
        pSettingsFile.close();
      } else {
        LOGGER(ERROR, false, "Failed to open Settings file.");
      }
      ///////////////////////////////////////////////////
      struct tm currentTime;

      LOGGER(INFO, false, "Getting Datetime from SD Card...");

      File pTimeFile = SD.open("/time", FILE_READ); // Read Time file
      if (pTimeFile) {
        setenv("TZ", TIMEZONE, 1);
        tzset();

        LOGGER(INFO, false, "Timezone setted.");

        SetCurrentDatetime(pTimeFile.readStringUntil('\n').toInt());

        LOGGER(INFO, false, "Current datetime setted.");

        GetLocalTimeNow(&currentTime);

        LOGGER(INFO, false, "Current Datetime: %02d/%02d/%04d %02d:%02d:%02d.", currentTime.tm_mday, currentTime.tm_mon + 1, currentTime.tm_year + 1900, currentTime.tm_hour, currentTime.tm_min, currentTime.tm_sec);

        pTimeFile.close();
      }
      ///////////////////////////////////////////////////
      char cBufferFilePath[29];
      snprintf(cBufferFilePath, sizeof(cBufferFilePath), "/metrics/metrics_%02d_%04d.txt", currentTime.tm_mon + 1, currentTime.tm_year + 1900);

      File pMetricsFile = SD.open(cBufferFilePath, FILE_READ);
      if (pMetricsFile) {
        size_t nFileSize = pMetricsFile.size();
        const size_t nBufSize = MAX_GRAPH_MARKS * MAX_GRAPH_MARKS_LENGTH;
        char cBuf[nBufSize + 1];
        size_t nReadSize = (nFileSize > nBufSize) ? nBufSize : nFileSize;

        pMetricsFile.seek(nFileSize - nReadSize);
        pMetricsFile.read((uint8_t*)cBuf, nReadSize);

        cBuf[nReadSize] = '\0';

        pMetricsFile.close();

        uint8_t nLinesRead = 0;
        char* pEnd = cBuf + nReadSize;

        while (nLinesRead < MAX_GRAPH_MARKS) {
          char* pNewline = (char*)memrchr(cBuf, '\n', pEnd - cBuf);
          char* pLineStart = pNewline ? pNewline + 1 : cBuf;

          if (pNewline)
            *pNewline = '\0';

          char* pCR = pLineStart + strlen(pLineStart) - 1;
          if (pCR >= pLineStart && *pCR == '\r')
            *pCR = '\0';

          if (*pLineStart != '\0') {
            strncpy(g_strArrayGraphData[MAX_GRAPH_MARKS - 1 - nLinesRead], pLineStart, MAX_GRAPH_MARKS_LENGTH - 1);
            g_strArrayGraphData[MAX_GRAPH_MARKS - 1 - nLinesRead][MAX_GRAPH_MARKS_LENGTH - 1] = '\0';

            nLinesRead++;
          }

          if (!pNewline || pNewline <= cBuf)
            break;

          pEnd = pNewline;
        }
      }
    } else {
      LOGGER(ERROR, false, "SD initialization failed. Settings & Time will not be loaded, but the system will not restart to avoid unexpected relay behavior.");
    }
  });

  LOGGER(INFO, true, "Loading Profile...");

  LoadProfile(g_nCurrentProfile);  // LOAD PROFILE VALUES

  ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - ((g_nLightBrightness < 401) ? 0 : g_nLightBrightness));  // WARNING: Hardcode offset

  LOGGER(INFO, true, "Initializing Wifi...");

#ifdef ENABLE_AP_ALWAYS
  WiFi.mode(WIFI_AP_STA); // Set dual mode, Access Point & Station

  WiFi.softAP(ACCESSPOINT_NAME);

  LOGGER(INFO, true, "Access Point active. AP IP: %s", WiFi.softAPIP().toString().c_str());
#else
  WiFi.mode(WIFI_STA);  // Only Station mode
#endif

  SafeSDAccess([&]() {
    if (!g_bIsSDInit)
      return;

      WiFi.begin(g_cSSID, g_cSSIDPWD);

      uint8_t nConnectTrysCount = 0;

      while (nConnectTrysCount < WIFI_MAX_RETRYS && WiFi.status() != WL_CONNECTED) {
        nConnectTrysCount++;

        delay(WIFI_RETRY_INTERVAL); // Wait before trying again
      }

      if (WiFi.status() == WL_CONNECTED)
        LOGGER(INFO, true, "Connected to Wifi SSID: %s PASSWORD: %s. IP: %s.", g_cSSID, g_cSSIDPWD, WiFi.localIP().toString().c_str());
      else
        LOGGER(ERROR, true, "Max Wifi reconnect attempts reached.");
  });

  LOGGER(INFO, true, "Creating Wifi reconnect task thread...");

  xTaskCreatePinnedToCore(Thread_WifiReconnect, "Wifi Reconnect Task", 4096, NULL, 1, &g_pWiFiReconnect, 0);
  vTaskSuspend(g_pWiFiReconnect); // Suspend the task as it's not needed right now

  LOGGER(INFO, true, "Setting up Web Server...");

  // Static files server
  for (uint8_t i = 0; i < (sizeof(g_strWebServerFiles) / sizeof(g_strWebServerFiles[0])); i++)
    g_pWebServer.serveStatic(g_strWebServerFiles[i], SD, g_strWebServerFiles[i]).setCacheControl("max-age=2592000");  // Cache it by 1 month

  // Static serving of the logs & metrics folder and all the files inside it
  g_pWebServer.serveStatic("/logs", SD, "/logs").setCacheControl("max-age=2592000, immutable"); // Cache it by 1 month
  g_pWebServer.serveStatic("/metrics", SD, "/metrics").setCacheControl("max-age=2592000, immutable"); // Cache it by 1 month

  // index.html server & Request handler
  g_pWebServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasArg("action")) {  // Process the request
      if (request->arg("action") == "restart") {
        LOGGER(INFO, true, "Restarting Controller by Web command.");

        delay(1000);

        ESP.restart();
      } else if (request->arg("action") == "cisl") {
        String strReturn = "La calibración falló. Intente nuevamente.";
        
        int16_t nLowerLevel = GetIrrigationReservoirLevel();
        if (nLowerLevel != -1) {
          g_nIrrigationReservoirLowerLevel = nLowerLevel;

          strReturn = "Se calibró el nivel Mínimo del Reservorio de Solución de Riego.";

          SaveSettings();
        }

        request->send(200, "text/plain", "MSG" + strReturn);
        return;
      } else if (request->arg("action") == "reload") {  // This is for reload Elements from the Panel based on selected Profile
        // =============== Profile Selector =============== //
        if (request->hasArg("profile")) {
          String strReturn = "";
          // =============== Profile Selector =============== //
          uint8_t nSelectedProfile = request->arg("profile").toInt();
          // If asked profile doesn't match with current profile, change to asked one.
          if (nSelectedProfile != g_nCurrentProfile) {
            if (g_nIrrigationDuration != 0 || g_bApplyFertilizers) {
              strReturn = "No es posible cambiar de Perfil en este momento ";

              if (g_nIrrigationDuration != 0)
                strReturn += "porque se está ejecutando un Riego.";
              else if (g_bApplyFertilizers)
                strReturn += "porque se están aplicando Fertilizantes.";

              request->send(200, "text/plain", "ERR0:" + String(g_nCurrentProfile) + ":" + strReturn);
              return;
            } else {
              SaveProfile(g_nCurrentProfile); // Final save of old Profile

              g_nCurrentProfile = nSelectedProfile;

              LoadProfile(g_nCurrentProfile); // Load values from new Profile

              // Reset some values
              g_nLastWateredHour = -1;
              g_nIrrigationDayCounter = 1;

              SaveProfile(g_nCurrentProfile);

              SaveSettings();

              strReturn += "profilechanged:" + String(g_nCurrentProfile) + ":";
            }
          }
          // If is just asking for profile information, return it.
          strReturn += "lightstartstop:" + String(g_nStartLightTime) + "," + String(g_nStopLightTime);
          strReturn += ":lb:" + String((g_nLightBrightness < 401) ? 0 : g_nLightBrightness);  // WARNING: Hardcode offset
          strReturn += ":ifm:" + String(g_nInternalFanMode);
          strReturn += ":ifts:" + String(g_nStartInternalFanTemperature);
          strReturn += ":rfm:" + String(g_nVentilationMode);
          strReturn += ":rfts:" + String(g_nStartVentilationTemperature);
          strReturn += ":rfhs:" + String(g_nStartVentilationHumidity);
          strReturn += ":fim:" + String(g_nFertilizerIncorporationMode);
          strReturn += ":idc:" + String(g_nIrrigationDayCounter);
          ///////////////////////////////////////////////////
          strReturn += ":ichart:";

          bool bFirst = true;

          for (const auto& Watering : g_vecWateringStages) {
            if (!bFirst)
              strReturn += ",";
            else
              bFirst = false;

            strReturn += String(Watering.Day) + "|" + String(Watering.TargetCC);

            for (uint8_t i = 0; i < MAX_FERTILIZER_PUMPS; ++i)
              strReturn += "|" + String(Watering.FertilizerToApply[i]);
          }

          request->send(200, "text/plain", "RELOAD" + strReturn);
          return;
        }
      } else if (request->arg("action") == "applyferts") {
        String strReturn = "No se pueden incorporar los Fertilizantes en este momento ";

        if (g_nIrrigationDuration == 0 && g_nTestPumpID == 0 && !g_bApplyFertilizers && !g_bManualMixing) {
          strReturn = "Se comenzará a incorporar los Fertilizantes.";

          g_bApplyFertilizers = true;
        } else {
          if (g_nIrrigationDuration != 0)
            strReturn += "porque se está ejecutando un Riego.";
          else if (g_nTestPumpID != 0)
            strReturn += "porque se está ejecutando una prueba de Caudal.";
          else if (g_bApplyFertilizers)
            strReturn += "porque se están aplicando Fertilizantes.";
          else if (g_bManualMixing)
            strReturn += "porque se está Mezclando la Solución de Riego.";
        }

        request->send(200, "text/plain", "MSG" + strReturn);
        return;
      } else if (request->arg("action").startsWith("testpump")) {
        String strReturn = "La bomba no se puede probar en este momento.";

        if (g_nIrrigationDuration == 0 && g_nTestPumpID == 0 && !g_bApplyFertilizers && !g_bManualMixing) {
          uint8_t nPumpID = request->arg("action").substring(8).toInt();

          if (nPumpID > 0) {
            g_nTestPumpID = nPumpID;  // From IRRIGATION_PUMP(4) to FERTILIZER_PUMP_2(7)
            strReturn = "La bomba estará encendida durante los próximos 60 segundos.";
          }
        }

        request->send(200, "text/plain", "MSG" + strReturn);
        return;
      } else if (request->arg("action") == "mixis") {
        String strReturn = "No es posible Mezclar en este momento.";

        if (g_nIrrigationDuration == 0 && g_nTestPumpID == 0 && !g_bApplyFertilizers && g_nIrrigationSolutionLevel > 0 && !g_bManualMixing) {
          g_bManualMixing = true;
          strReturn = "La bomba de Mezcla estará encendida durante los próximos " + String(TicksToSeconds(g_nMixingPumpDuration)) + " segundos.";
        }

        request->send(200, "text/plain", "MSG" + strReturn);
        return;
      } else if (request->arg("action") == "update") {  // This is for update Settings
        uint64_t nNewValue;
        String strReturn = "";
        // =============== Current DateTime =============== //
        if (request->hasArg("time")) {
          SetCurrentDatetime(request->arg("time").toInt());

          struct tm currentTime;
          GetLocalTimeNow(&currentTime);
          LOGGER(INFO, true, "New Datetime: %02d/%02d/%04d %02d:%02d:%02d.", currentTime.tm_mday, currentTime.tm_mon + 1, currentTime.tm_year + 1900, currentTime.tm_hour, currentTime.tm_min, currentTime.tm_sec);

          strReturn += "Se sincronizó la Fecha actual.\r\n";
        }
        // =============== Light Start =============== //
        if (CheckNSetValue(request, "lightstart", g_nStartLightTime, F("la Hora de Encendido de Luz"), strReturn))
          g_nEffectiveStartLights = (g_nStartLightTime == 24) ? 0 : g_nStartLightTime;  // Stores the effective light start hour, converting hour 24 to 0 (midnight)
        // =============== Light Stop =============== //
        if (CheckNSetValue(request, "lightstop", g_nStopLightTime, F("la Hora de Apagado de Luz"), strReturn))
          g_nEffectiveStopLights = (g_nStopLightTime == 24) ? 0 : g_nStopLightTime; // Stores the effective light stop hour, converting hour 24 to 0 (midnight)
        // =============== Light Brightness =============== //
        if (request->hasArg("lb")) {
          nNewValue = request->arg("lb").toInt();

          if (nNewValue != g_nLightBrightness) {
            g_nLightBrightness = (nNewValue < 401) ? 0 : nNewValue; // WARNING: Hardcode offset

            ledcWrite(S8050_PWM_PIN, S8050_MAX_VALUE - g_nLightBrightness);

            strReturn += "Se actualizó la Intensidad de Luz.\r\n";
          }
        }
        // =============== Crop Begin Date =============== //
        CheckNSetValue(request, "cb", g_nCropBegin, F("la Fecha de inicio de Cultivo"), strReturn);
        // =============== Current Irrigation Day =============== //
        CheckNSetValue(request, "idc", g_nIrrigationDayCounter, F("los Días de Riego transcurridos"), strReturn);
        // =============== Internal Fan Mode =============== //
        CheckNSetValue(request, "ifm", g_nInternalFanMode, F("el Modo de Ventilación Interna"), strReturn);
        // =============== Internal Fan Start =============== //
        CheckNSetValue(request, "ifts", g_nStartInternalFanTemperature, F("la Temperatura de Encendido del Ventilador Interno"), strReturn);
        // =============== Ventilation Fans Mode =============== //
        CheckNSetValue(request, "rfm", g_nVentilationMode, F("el Modo de Recirculación de Aire"), strReturn);
        // =============== Ventilation Temperature Start =============== //
        CheckNSetValue(request, "rfts", g_nStartVentilationTemperature, F("la Temperatura de Encendido de Recirculación"), strReturn);
        // =============== Ventilation Humidity Start =============== //
        CheckNSetValue(request, "rfhs", g_nStartVentilationHumidity, F("la Humedad de Encendido de la Recirculación"), strReturn);
        // =============== Fertilizers Incorporation Mode =============== //
        CheckNSetValue(request, "fim", g_nFertilizerIncorporationMode, F("el Modo de Incorporación de Fertilizantes"), strReturn);
        // =============== Irrigation Scheme =============== //
        if (request->hasArg("ichart")) {
          const String& refstrValues = request->arg("ichart");
          const char* strBuffer = refstrValues.c_str();
          std::vector<WateringData> vecNewWateringStages;

          while (*strBuffer) {
            char* cEndBuffer;
            WateringData pData = {};

            long nDay = strtol(strBuffer, &cEndBuffer, 10);

            if (*cEndBuffer != '|')
              break;

            strBuffer = cEndBuffer + 1;

            long nTargetCC = strtol(strBuffer, &cEndBuffer, 10);

            if (*cEndBuffer != '|')
              break;

            strBuffer = cEndBuffer + 1;

            if ((nDay > 0 && nDay <= UINT16_MAX) && (nTargetCC >= 0 && nTargetCC <= UINT16_MAX)) {
              pData.Day = static_cast<uint16_t>(nDay);
              pData.TargetCC = static_cast<uint16_t>(nTargetCC);

              for (uint8_t i = 0; i < MAX_FERTILIZER_PUMPS; ++i) {
                float fFertilizerCC = strtof(strBuffer, &cEndBuffer);

                if (fFertilizerCC < 0.0f || !isfinite(fFertilizerCC))
                  fFertilizerCC = 0.0f;

                pData.FertilizerToApply[i] = fFertilizerCC;

                if (*cEndBuffer == '|')
                  strBuffer = cEndBuffer + 1;
                else
                  strBuffer = cEndBuffer;
              }

              vecNewWateringStages.push_back(pData);
            }

            if (*cEndBuffer == ',')
              strBuffer = cEndBuffer + 1;
            else
              break;
          }

          if (vecNewWateringStages != g_vecWateringStages) {
            g_vecWateringStages = vecNewWateringStages;

            strReturn += "Se actualizó el Esquema de Riego Y Fertilización.\r\n";
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
        CheckNSetValue(request, "temphys", g_nTemperatureStopHysteresis, F("la Histéresis de Apagado por Temperatura"), strReturn);
        // =============== Environment Humidity Hysteresis =============== //
        CheckNSetValue(request, "humhys", g_nHumidityStopHysteresis, F("la Histéresis de Apagado por Humedad"), strReturn);
        // =============== Irrigation Pump CC Flow Per Minute =============== //
        CheckNSetValue(request, "ifpm", g_nIrrigationFlowPerMinute, F("el Caudal por Minuto de Bomba de Riego"), strReturn);
        // =============== Each Fertilizer Pump CC Flow Per Minute =============== //
        for (uint8_t i = 0; i < MAX_FERTILIZER_PUMPS; i++) {
          String strArg = "pumpfpm" + String(i);
          if (request->hasArg(strArg)) {
            nNewValue = request->arg(strArg).toInt();

            if (nNewValue != g_nFertilizersPumpsFlowPerMinute[i]) {
              g_nFertilizersPumpsFlowPerMinute[i] = nNewValue;

              strReturn += "Se actualizó el Caudal por Minuto de Bomba de " + String((i == 0) ? "Reductor de pH" : (i == 1 ? "Fertilizante de Vegetativo" : "Fertilizante de Floración")) + ".\r\n";
            }
          }
        }
        // =============== Mixing Pump Duration =============== //
        if (request->hasArg("mixdur")) {
          nNewValue = SecondsToTicks(request->arg("mixdur").toInt());

          if (nNewValue != g_nMixingPumpDuration) {
            g_nMixingPumpDuration = nNewValue;

            strReturn += "Se actualizó la Duración de Mezclado de Solución de Riego.\r\n";
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
          strReturn += "Se intentará conectar a la nueva Red.";
#else
          strReturn += "Se intentará conectar a la nueva Red, de no ser posible; se iniciará una Red Wifi (" + String(ACCESSPOINT_NAME) + ") para poder reconfigurar el controlador.";
#endif
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

          SaveProfile(g_nCurrentProfile);

          if (bWiFiChanges) { // After send response to web client, Try reconnect to Wifi if is required
            LOGGER(WARN, true, "Disconnecting Wifi to start connection to new SSID...");

            WiFi.disconnect(false); // First disconnect from current Network (Arg false to just disconnect the Station, not the AP)

            if (eTaskGetState(g_pWiFiReconnect) == eRunning)  // Then check if task g_pWiFiReconnect is running. If it is, Suspend it to eventually Resume it with the new SSID and SSID Password
              vTaskSuspend(g_pWiFiReconnect);
          }

          return;
        }
      } else if (request->arg("action") == "refresh") { // This is for refresh Panel values constantly
        // ================================================== Environment Section ================================================== //
#ifdef USE_DHT11
        String strResponse = String(g_nEnvironmentTemperature) + ":" + String(g_nEnvironmentHumidity);
#elif defined(USE_DHT22)
        String strResponse = String(g_fEnvironmentTemperature, 1) + ":" + String(g_fEnvironmentHumidity, 1);
#endif
        // ================================================== Irrigation Solution Level Section ================================================== //
        strResponse += ":" + String(g_nIrrigationSolutionLevel);
        // ================================================== Soil Section ================================================== //
        strResponse += ":";

        bool bFirst = true;

        for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++) {
          if (!bFirst)
            strResponse += ",";
          else
            bFirst = false;

          strResponse += String(g_nSoilsHumidity[i]);
        }
        // ================================================== Current Time Section ================================================== //
        time_t pTimeNow = time(nullptr);

        strResponse += ":" + String(pTimeNow);
        // ================================================== Fans Rest State Section ================================================== //
        strResponse += ":";

        if (g_nFansRestTimeStartedAt != 0)
          strResponse += String(TicksToSeconds(g_nFansRestDuration - (millis64() - g_nFansRestTimeStartedAt)));
        else
          strResponse += "0";

        // Internal Fan
        strResponse += ":" + String(digitalRead(RELAYS_MAP[INTERNAL_FAN].Pin)); // 1 = stopped, 0 turn on

        // Ventilation
        strResponse += ":" + String(digitalRead(RELAYS_MAP[VENTILATION_FANS].Pin));  // 1 = stopped, 0 turn on
        // ================================================== Irrigation Section ================================================== //
        strResponse += ":" + String(g_nIrrigationDayCounter);
        strResponse += ":" + String(g_nIrrigationDuration);  // Ticks
        // ================================================== Firmware Versioning Section ================================================== //
        strResponse += ":" + String(FIRMWAREVERSION);
        // ================================================== Graph Section ================================================== //
        if (g_strArrayGraphData[0][0] != '\0') {
          strResponse += ":";

          bool bFirst = true;

          for (int8_t i = MAX_GRAPH_MARKS - 1; i >= 0; i--) {
            if (g_strArrayGraphData[i][0] != '\0') {

              if (!bFirst)
                strResponse += ",";
              else
                bFirst = false;

              strResponse += g_strArrayGraphData[i];
            }
          }
        }
        // ========================================================================================================================= //
        /*
          Response structure example: each data[X] is divided by ':'
          data[0] → Environment Temperature
          data[1] → Environment Humidity
          data[2] → Irrigation Solution Level
          data[3] → <Soil Moistures values Array> Example: SOIL 1 MOISTURE VALUE,SOIL 2 MOISTURE VALUE
          data[4] → Current Timestamp
          data[5] → Fans Rest time Remaining
          data[6] → Internal Fan State
          data[7] → Ventilation Fan State
          data[8] → Irrigation Day Counter
          data[9] → Irrigation Time Remaining
          data[10] → Firmware Version
          data[11] → <History Chart values Array> Example: Unix Timestamp|Environment Temperature|Environment Humidity|VPD|<Soil Moistures values Array>
        */
        request->send(200, "text/plain", "REFRESH" + strResponse);
        return;
      } else if (request->arg("action") == "list") {  // This returns the file list in logs folder
        if (request->hasArg("logs") || request->hasArg("metrics")) {
          SafeSDAccess([&]() {
            if (g_bIsSDInit) {
              bool bFirst = true;
              String strFileName, strResponse;
              String strFolder = request->hasArg("logs") ? "logs" : "metrics";
              File pLogsDir = SD.open("/" + strFolder);
              File pFile = pLogsDir.openNextFile();

              while (pFile) {
                strFileName = String(pFile.name());

                if (!pFile.isDirectory()) {
                  if (!bFirst)
                    strResponse += ":";
                  else
                    bFirst = false;

                  int nLastSlash = strFileName.lastIndexOf('/');
                  if (nLastSlash >= 0)
                    strResponse += strFileName.substring(nLastSlash + 1);
                  else
                    strResponse += strFileName;
                }

                pFile.close();

                pFile = pLogsDir.openNextFile();
              }

              pLogsDir.close();

              request->send(200, "text/plain", strResponse);
            } else {
              request->send(500, "text/plain", "No hay una Tarjeta SD conectada.");
            }
          });
        }

        return;
      }
    } else {  // Return Panel content
      SafeSDAccess([&]() {
        AsyncWebServerResponse* pResponse;

        if (g_bIsSDInit)
          pResponse = request->beginResponse(SD, "/www/index.html", "text/html", false, HTMLProcessor);
        else
          pResponse = request->beginResponse(500, "text/plain", "No hay una Tarjeta SD conectada.");

        request->send(pResponse);
      });

      return;
    }

    request->send(500, "text/plain", "HTTP 500");
  });

  g_pWebServer.onNotFound([](AsyncWebServerRequest* request) { request->send(404, "text/plain", "HTTP 404"); });

  g_pWebServer.on("/ota", HTTP_POST, [](AsyncWebServerRequest* request) {
    bool bUpdate = !Update.hasError();

    if (bUpdate) {
      time_t pTimeNow = time(nullptr);
      char cBuffer[12];
      snprintf(cBuffer, sizeof(cBuffer), "%lu", (long)pTimeNow);

      WriteToSD("/time", cBuffer, false); // Write current time to SD Card

      SaveSettings();

      SaveProfile(g_nCurrentProfile);

      LOGGER(INFO, true, "Restarting Controller to do a Firmware Update.");

      delay(1000);

      ESP.restart();
    }
  }, [](AsyncWebServerRequest* request, String strFileName, size_t nIndex, uint8_t* nData, size_t nLen, bool bFinal) {
    static bool bUpdateError = false;

    if (!nIndex) {
      bUpdateError = false;

      Update.abort();

      LOGGER(INFO, true, "Updating Firmware. File: %s", strFileName.c_str());

      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        bUpdateError = true;

        LOGGER(ERROR, true, "Firmware update failed. Error: %s", Update.errorString());
      }
    }

    if (!bUpdateError && Update.write(nData, nLen) != nLen) {
      bUpdateError = true;

      LOGGER(ERROR, true, "Firmware update failed. Error: %s", Update.errorString());
    } else {
      static uint8_t nLastPercent = 0;
      uint8_t nPercent = (Update.progress() * 100) / Update.size();

      if (nPercent != nLastPercent) {
        nLastPercent = nPercent;
        LOGGER(INFO, true, "Firmware update written: %d%%", nPercent);
      }
    }

    if (bFinal) {
      if (!bUpdateError && Update.end(true))
        LOGGER(INFO, true, "Firmware Update successfully.");
      else
        LOGGER(ERROR, true, "Firmware update failed. Error: %s", Update.errorString());
    }
  });

  g_pWebServer.on("/upload-clean", HTTP_POST, [](AsyncWebServerRequest* request) {
    SafeSDAccess([&]() {
      File pWWW = SD.open("/www");
      std::vector<String> vecTmpFiles;

      while (File pFile = pWWW.openNextFile()) {
        const char* strFileName = pFile.name();
        size_t nLen = strlen(strFileName);

        if (nLen > 4 && strcmp(strFileName + nLen - 4, ".tmp") == 0)
          vecTmpFiles.emplace_back(pFile.path());

        pFile.close();
      }

      pWWW.close();

      for (auto& strTmpFull : vecTmpFiles)
        SD.remove(strTmpFull.c_str());

      LOGGER(INFO, false, "Cancelled: %d temporary files removed.", vecTmpFiles.size());
    });

    request->send(200, "text/plain", "OK.");
  });

  g_pWebServer.on("/upload", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (request->_tempObject)
      request->send(500, "text/plain", "SD open failed.");
    else
      request->send(200, "text/plain", "OK");
  }, [](AsyncWebServerRequest* request, String strFileName, size_t nIndex, uint8_t* nData, size_t nLen, bool bFinal) {
    if (request->_tempObject)
      return;

    SafeSDAccess([&]() {
      String strTmpPath = String("/www/") + strFileName + ".tmp";

      if (!nIndex) {
        SD.remove(strTmpPath);

        auto it = g_mapUploadFiles.find(strTmpPath);
        if (it != g_mapUploadFiles.end()) {
          it->second.close();

          g_mapUploadFiles.erase(it);

          LOGGER(WARN, false, "Recovered aborted upload: %s", strFileName.c_str());
        }

        LOGGER(INFO, false, "Uploading: %s", strFileName.c_str());

        File pFile = SD.open(strTmpPath, FILE_WRITE);
        if (!pFile) {
          request->_tempObject = (void*)1;

          LOGGER(ERROR, false, "Failed to open: %s", strTmpPath.c_str());

          return;
        }

        g_mapUploadFiles[strTmpPath] = pFile;
      }

      auto it = g_mapUploadFiles.find(strTmpPath);
      if (it != g_mapUploadFiles.end()) {
        it->second.write(nData, nLen);

        if (bFinal) {
          it->second.close();

          g_mapUploadFiles.erase(it);

          const char* strExpectedSize = request->getHeader("File-Size") ? request->getHeader("File-Size")->value().c_str() : nullptr;
          if (strExpectedSize) {
            File pFile = SD.open(strTmpPath, FILE_READ);

            if (pFile && pFile.size() != (size_t)atoi(strExpectedSize)) {
              SD.remove(strTmpPath);

              request->_tempObject = (void*)1;

              LOGGER(ERROR, false, "File size mismatch: %s", strFileName.c_str());
            } else {
              LOGGER(INFO, false, "Temporary file saved: %s", strTmpPath.c_str());
            }

            if (pFile)
              pFile.close();
          } else {
            LOGGER(WARN, false, "Temporary file: %s saved, but missing File-Size Header.", strFileName.c_str());
          }
        }
      }
    });
  });

  g_pWebServer.on("/upload-commit", HTTP_POST, [](AsyncWebServerRequest* request) {
    bool bAllOk = false;

    SafeSDAccess([&]() {
      File pWWW = SD.open("/www");
      std::vector<String> vecTmpFiles;

      while (File pFile = pWWW.openNextFile()) {
        const char* strFileName = pFile.name();
        size_t nLen = strlen(strFileName);

        if (nLen > 4 && strcmp(strFileName + nLen - 4, ".tmp") == 0)
          vecTmpFiles.emplace_back(pFile.path());

        pFile.close();
      }

      pWWW.close();

      bAllOk = true;

      for (auto& strTmpFull : vecTmpFiles) {
        String strFinalFull = strTmpFull.substring(0, strTmpFull.length() - 4);

        SD.remove(strFinalFull);

        if (!SD.rename(strTmpFull, strFinalFull)) {
          LOGGER(ERROR, false, "File rename failed: %s", strTmpFull.c_str());

          bAllOk = false;
        }
      }
    });

    request->send(200, "text/plain", bAllOk ? "Actualización completa." : "Error en el reemplazo.");
  });

  g_pWebServer.begin();

  LOGGER(INFO, true, "Web Server Started at Port: %d.", WEBSERVER_PORT);
}

void loop() {
  static uint64_t nLastSecondTick = 0;
  uint64_t nCurrentMillis = millis64();

  if ((nCurrentMillis - nLastSecondTick) >= 1000) { // Check if 1 second has passed since the last tick to perform once-per-second tasks
    nLastSecondTick = nCurrentMillis;
    time_t pTimeNow = time(nullptr);
    struct tm currentTime;
    localtime_r(&pTimeNow, &currentTime);
    // ================================================== Wifi Section ================================================== //
    {
      static uint64_t nLastReconnectAttempt = 0;

      if (eTaskGetState(g_pWiFiReconnect) == eSuspended && WiFi.status() != WL_CONNECTED && (nCurrentMillis - nLastReconnectAttempt) >= WIFI_RETRY_CONNECT_INTERVAL) { // If is not connected to Wifi and is not currently running a reconnect trask, start it
        nLastReconnectAttempt = nCurrentMillis;

        vTaskResume(g_pWiFiReconnect);
      }
    }
    // ================================================== Time Section ================================================== //
    {
      static uint64_t nLastWriteTick = 0;

      if ((nCurrentMillis - nLastWriteTick) >= TIME_SAVE_INTERVAL) {
        nLastWriteTick = nCurrentMillis;

        char cBuffer[12];
        snprintf(cBuffer, sizeof(cBuffer), "%lu", (long)pTimeNow);
        WriteToSD("/time", cBuffer, false); // Write current time to SD Card
      }
    }
    // ================================================== Environment Section ================================================== //
    {
#ifdef USE_DHT11
      byte bTemperature = 0, bHumidity = 0;
      uint8_t nError = g_pDHT11.read(&bTemperature, &bHumidity, NULL);

      if (nError == SimpleDHTErrSuccess) {
        g_nEnvironmentTemperature = static_cast<uint8_t>(bTemperature);
        g_nEnvironmentHumidity = static_cast<uint8_t>(bHumidity);
      } else {
        LOGGER(ERROR, true, "DHT11 reads failed. Error: %d", nError);
      }
#elif defined(USE_DHT22)
      static uint64_t nLastEnvironmentCheck = 0;
      static uint64_t nDHTReadyAt = 2000;
      static bool bPowerStabilized = false;

      if (bPowerStabilized) {
        if (nCurrentMillis >= nDHTReadyAt) {  // After set the pin to LOW, put again to HIGH and wait 2 seconds
          nDHTReadyAt = nCurrentMillis + 2000;

          digitalWrite(DHT_VCC_PIN, HIGH);

          bPowerStabilized = false;
        }
      } else if (nCurrentMillis >= nDHTReadyAt && (nCurrentMillis - nLastEnvironmentCheck) >= 2000) {
        nLastEnvironmentCheck = nCurrentMillis;

        float fTemperature = 0.0f, fHumidity = 0.0f;
        uint8_t nError = g_pDHT22.read2(&fTemperature, &fHumidity, NULL);

        if (nError == SimpleDHTErrSuccess) {
          g_fEnvironmentTemperature = fTemperature;
          g_fEnvironmentHumidity = fHumidity;
        } else {  // Set pin to LOW and wait 500ms
          LOGGER(ERROR, true, "DHT22 reads failed. Error: %d", nError);

          digitalWrite(DHT_VCC_PIN, LOW);

          nDHTReadyAt = nCurrentMillis + 500;

          bPowerStabilized = true;
        }
      }
#endif
    }
    // ================================================== Soil Section ================================================== //
    {
      static bool bFirstCheck = false;

      if (!bFirstCheck) {
        bFirstCheck = true;

        CheckSoilHumidity();
      }
    }
    // ================================================== Irrigation Solution Level Section ================================================== //
    {
      static uint64_t nLastReservoirLevelCheck = 0;

      if (g_nIrrigationSolutionLevel == 0 || (nCurrentMillis - nLastReservoirLevelCheck) >= CHECK_RESERVOIR_LEVEL_INTERVAL) {
        nLastReservoirLevelCheck = nCurrentMillis;

        CheckReservoirLevel();
      }
    }
    // ================================================== Lights Sections ================================================== //
    {
      if ((g_nStartLightTime > 0 || g_nStopLightTime > 0) &&  // Check if either the light start time or stop time is set (greater than 0)
          (g_nEffectiveStartLights < g_nEffectiveStopLights && currentTime.tm_hour >= g_nEffectiveStartLights && currentTime.tm_hour < g_nEffectiveStopLights) || // Normal case: light start time is before stop time (e.g., from 7 AM to 7 PM)
          (g_nEffectiveStartLights >= g_nEffectiveStopLights && (currentTime.tm_hour >= g_nEffectiveStartLights || currentTime.tm_hour < g_nEffectiveStopLights))) {  // Special case: light schedule crosses midnight (e.g., from 8 PM to 6 AM)
        if (digitalRead(RELAYS_MAP[LIGHTS].Pin)) {
          digitalWrite(RELAYS_MAP[LIGHTS].Pin, RELAY_PIN_ON);

          LOGGER(INFO, true, "Lights Started.");
        }
      } else {  // If Current Time is out of ON range, turn it OFF
        if (!digitalRead(RELAYS_MAP[LIGHTS].Pin)) {
          digitalWrite(RELAYS_MAP[LIGHTS].Pin, RELAY_PIN_OFF);

          LOGGER(INFO, true, "Lights Stopped.");
        }
      }
    }
    // ================================================== Fertilizers Incorporation Section ================================================== //
    {
      if (g_bApplyFertilizers) {
        static FertilizerIncorporationStage Stages[MAX_FERTILIZER_PUMPS] = {};
        static uint8_t nMaxStages = 0;

        if (nMaxStages == 0) {
          bool bCanIncorporateFerts = false;

          for (int8_t i = g_vecWateringStages.size() - 1; i >= 0; --i) {
            const auto& Watering = g_vecWateringStages[i];

            if (!bCanIncorporateFerts && g_nIrrigationDayCounter >= Watering.Day && Watering.TargetCC > 0) {
              if (g_nFertilizerIncorporationMode == 0) {  // Permissive mode: Incorporate ferts from last known day with ferts values
                bCanIncorporateFerts = true;
              } else if (g_nFertilizerIncorporationMode == 1) { // Strict mode: Incorporate ferts from current day (If have defined)
                for (uint8_t j = 0; j < MAX_FERTILIZER_PUMPS; ++j) {
                  if (Watering.FertilizerToApply[j] > 0.001f) {
                    bCanIncorporateFerts = true;

                    break;
                  }
                }
              }
            }

            if (!bCanIncorporateFerts && g_nIrrigationDayCounter >= Watering.Day)
              break;

            bool bFertilizerFound = false;

            for (uint8_t j = 0; j < MAX_FERTILIZER_PUMPS; ++j) {  // Check if really have values of ferts to be incorporated
              if (Watering.FertilizerToApply[j] > 0.001f) {
                bFertilizerFound = true;

                break;
              }
            }

            if (bCanIncorporateFerts && bFertilizerFound) {
              for (uint8_t j = 0; j < MAX_FERTILIZER_PUMPS; ++j) {
                float fCCToApply = Watering.FertilizerToApply[j];

                if (fCCToApply > 0.001f) {
                  uint64_t nDuration = static_cast<uint64_t>((fCCToApply * FLOW_TEST_DURATION) / g_nFertilizersPumpsFlowPerMinute[j] + 0.5f); // WARNING: Hardcode offset for round
                  if (nDuration == 0)
                    nDuration = 1;

                  Stages[nMaxStages++] = { static_cast<uint8_t>(FERTILIZER_PUMP_0 + j), nDuration };

                  LOGGER(INFO, true, "Preparing to apply %.1fcc of %s.", fCCToApply, RELAYS_MAP[FERTILIZER_PUMP_0 + j].Name);
                }
              }

              break;
            }
          }

          if (nMaxStages == 0) {
            g_bApplyFertilizers = false;

            LOGGER(WARN, true, "An attempt was made to incorporate Fertilizers, but the conditions were not met.");
          }
        } else {
          if (PowerSupplyControl(true)) {
            static uint64_t nFertilizersTimer = 0;

            if (nFertilizersTimer == 0) {
              nFertilizersTimer = nCurrentMillis;
            } else {
              static bool bPowerStabilized = false;

              if (!bPowerStabilized && (nCurrentMillis - nFertilizersTimer) >= SECONDS_WAIT_AFTER_TURN_ON_POWER_SUPPLY) {
                bPowerStabilized = true;
              } else if (bPowerStabilized) {
                static uint8_t nStage = 0;

                if (digitalRead(Stages[nStage].Pin)) {
                  digitalWrite(Stages[nStage].Pin, RELAY_PIN_ON);

                  nFertilizersTimer = nCurrentMillis;

                  LOGGER(INFO, true, "%s Started.", RELAYS_MAP[Stages[nStage].Pin].Name);
                } else {
                  if ((nCurrentMillis - nFertilizersTimer) >= Stages[nStage].Duration) {
                    digitalWrite(Stages[nStage].Pin, RELAY_PIN_OFF);

                    LOGGER(INFO, true, "%s Stopped.", RELAYS_MAP[Stages[nStage].Pin].Name);

                    nStage++;

                    if (nStage == nMaxStages) {
                      LOGGER(INFO, true, "Fertilizers Incorporation completed.");

                      PowerSupplyControl(false);

                      bPowerStabilized = false;
                      nStage = 0;
                      memset(Stages, 0, sizeof(Stages));
                      nMaxStages = 0;
                      nFertilizersTimer = 0;
                      g_bApplyFertilizers = false;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    // ================================================== Irrigation Section ================================================== //
    {
      static bool bIsTheLastPulse = false;
      static bool bApplyIrrigation = false;
      static uint8_t nTotalPulses = 0;  // Not need clean
      static uint8_t nCurrentPulse = 0; // It is over write in for loop call; Not need clean.
      static uint8_t nCurrentPulseHour = 0;

      if (!bApplyIrrigation) { // Si no se está aplicando un Riego; Verificar si se puede aplicar un Riego
        static int8_t nLastKnownCurrentProfile = -1;

        if (nLastKnownCurrentProfile == -1) // Si todavía no se asignó un valor (Porque es el primer inicio; Asignarlo)
          nLastKnownCurrentProfile = g_nCurrentProfile;

        if (nLastKnownCurrentProfile != g_nCurrentProfile) {  // El perfil cambió desde el último check; Hay que reiniciar variables
          nCurrentPulseHour = 0;
          bIsTheLastPulse = false;

          nLastKnownCurrentProfile = g_nCurrentProfile;
        }

        if (((currentTime.tm_hour - g_nLastWateredHour + 24) % 24) > 0 && !bIsTheLastPulse && !digitalRead(RELAYS_MAP[LIGHTS].Pin)) {  // Cada Hora en punto verificar si se puede Aplicar un Pulso de Riego
          uint8_t nPulseInterval = (g_nCurrentProfile == 1 /*Flowering*/) ? 2 : 1 /*Vegetative*/;
          uint8_t nStartIrrigationHour = (g_nEffectiveStartLights + 2) % 24;
          uint8_t nStopIrrigationHour = (g_nEffectiveStopLights - 2 + 24) % 24;

          nTotalPulses = ((nStopIrrigationHour - nStartIrrigationHour + 24) % 24) / nPulseInterval;

          CheckReservoirLevel();

          // Permitir regar si: se están incorporando los fertilizantes. Ni si se está haciendo una prueba de flujo de las Bombas. Ni si se está mezclando la solución de Riego. Ó se está ejecutando un riego.
          // NOTE: Estos checks son livianos así que por esa parte no hay Drama. En cambio, el check para verificar si TargetCC es > 0 es algo más pesado; Así que lo pongo por separado luego de hacer estos checks livianos.
          bApplyIrrigation = nTotalPulses > 0 &&  // Si al menos hay un pulso de Riego posible. (Tiene que haber 5 horas de luz minimo; Porque son 2 horas de encendido previo necesario, 2 al menos 2 horas restantes de luz encendida)
                            !g_bApplyFertilizers && // Si no se están incorporando Fertilizantes.
                            g_nTestPumpID == 0 && // Si no se está probando ninguna Bomba.
                            !g_bManualMixing && // Si no se está mezclando la Solución de Riego.
                            g_nIrrigationFlowPerMinute > 0 && // Si se definipo el Flujo por minutos de la Bomba de Riego.
                            g_nIrrigationSolutionLevel > 0;  // Si el nivel de Solución de Riego es mayor que 0

          if (bApplyIrrigation) { // Si hasta ahora si se puede aplicar un Pulso de Riego; Verificar si hay definido un TargetCC de Riego > 0
            uint16_t nLastKnownCC = 0;

            for (const auto& Watering : g_vecWateringStages) {
              if (g_nIrrigationDayCounter >= Watering.Day)
                nLastKnownCC = Watering.TargetCC;
              else
                break;
            }

            if (nLastKnownCC > 0) {
              for (nCurrentPulse = 0; nCurrentPulse < nTotalPulses; nCurrentPulse++) {  // Iterar por el total de Pulsos de Riego disponibles
                nCurrentPulseHour = (nStartIrrigationHour + nCurrentPulse * nPulseInterval) % 24; // Calcular la hora exacta de cada Pulso de Riego (Los Pulsos de Riego se hacen a la Hora en punto)

                if (nCurrentPulseHour == currentTime.tm_hour) {  // En la Hora actual, es posible hacer un Pulso de Riego.
                  g_nIrrigationDuration = ((static_cast<float>(nLastKnownCC) / nTotalPulses) * FLOW_TEST_DURATION) / g_nIrrigationFlowPerMinute; // Definir el tiempo que la Bomba de Riego tiene que estar encendida

                  if (nCurrentPulse == (nTotalPulses - 1))  // Verifica si es el último Pulso de Riego del Día
                    bIsTheLastPulse = true;

                  break;
                }
              }
            } else {  // No hay un TargetCC definido; Por lo que no se puede iniciar un Pulso de Riego.
              LOGGER(INFO, true, "Irrigation pulse skipped (No CC defined).");

              g_nLastWateredHour = currentTime.tm_hour;  // Se marca esta Hora cómo regada; Para no volver a verificar hasta la próxima

              SaveProfile(g_nCurrentProfile);

              bApplyIrrigation = false;
            }
          } else {  // No se puede iniciar un Pulso de Riego. Por X motivo
            if (g_nIrrigationSolutionLevel == 0 || g_nIrrigationFlowPerMinute == 0) {  // No hay Solución de Riego, Avisar por Whatsapp
              if (g_nIrrigationSolutionLevel == 0)
                SendNotification(String("No hay suficiente Solución de Riego para hacer este Pulso.").c_str());

              if (g_nIrrigationFlowPerMinute == 0)
                SendNotification(String("No se definió el Caudal de la Bomba de Riego.").c_str());

              // Estos 2 casos son bastante críticos, así que marcar la Hora cómo Regada. Además si no lo hiciera, SendNotification spamearía.
              g_nLastWateredHour = currentTime.tm_hour;  // Se marca esta Hora cómo regada; Para no volver a verificar hasta la próxima

              SaveProfile(g_nCurrentProfile);
            }

            bApplyIrrigation = false;
          }

          bApplyIrrigation = bApplyIrrigation && g_nIrrigationDuration > 0; // Finalmente si hasta ahora todos los checks fueron pasados exitosamente, verificar si g_nIrrigationDuration es > 0 quiere decir que ya pasó por for (uint8_t nCurrentPulse = 0; nCurrentPulse < nTotalPulses; nCurrentPulse++) y se asignó la duración del Pulso de Riego a hacer a continuación.
        }
      } else if (bApplyIrrigation) {  // Si se está aplicando un Riego; Verificar si ya se puede dejar de regar.
        if (PowerSupplyControl(true)) {
          static uint64_t nIrrigationTimer = 0;

          if (nIrrigationTimer == 0) {
            nIrrigationTimer = nCurrentMillis;
          } else {
            static bool bPowerStabilized = false;

            if (!bPowerStabilized && (nCurrentMillis - nIrrigationTimer) >= SECONDS_WAIT_AFTER_TURN_ON_POWER_SUPPLY) {
              bPowerStabilized = true;
            } else if (bPowerStabilized) {
              static uint8_t nStage = 0;

              switch (nStage) {
                case 0: // Mix Irrigation Solution
                  {
                    if (digitalRead(RELAYS_MAP[MIXING_PUMP].Pin)) {
                      digitalWrite(RELAYS_MAP[MIXING_PUMP].Pin, RELAY_PIN_ON);

                      nIrrigationTimer = nCurrentMillis;

                      LOGGER(INFO, true, "Mixing Pump Started.");
                    } else {
                      if ((nCurrentMillis - nIrrigationTimer) >= g_nMixingPumpDuration /*Ticks*/) {
                        digitalWrite(RELAYS_MAP[MIXING_PUMP].Pin, RELAY_PIN_OFF);

                        LOGGER(INFO, true, "Mixing Pump Stopped.");

                        nStage++;
                      }
                    }
                  }
                  break;
                case 1: // Apply Irrigation Solution
                  {
                    if (digitalRead(RELAYS_MAP[IRRIGATION_PUMP].Pin)) {
                      digitalWrite(RELAYS_MAP[IRRIGATION_PUMP].Pin, RELAY_PIN_ON);

                      nIrrigationTimer = nCurrentMillis;

                      LOGGER(INFO, true, "Irrigation Pump Started. Irrigation Info: Pulse Number: %d/%d Current Hour: %d Pulse Duration: %d seconds.", (nCurrentPulse + 1), nTotalPulses, nCurrentPulseHour, TicksToSeconds(g_nIrrigationDuration));
                    } else {
                      if ((nCurrentMillis - nIrrigationTimer) >= g_nIrrigationDuration /*Ticks*/) {
                        digitalWrite(RELAYS_MAP[IRRIGATION_PUMP].Pin, RELAY_PIN_OFF);

                        LOGGER(INFO, true, "Irrigation Pump Stopped. Irrigation Finished.");

                        PowerSupplyControl(false);

                        CheckReservoirLevel();

                        CheckSoilHumidity();

                        bPowerStabilized = false;
                        nStage = 0;
                        nIrrigationTimer = 0;
                        g_nIrrigationDuration = 0;
                        bApplyIrrigation = false;
                        g_nLastWateredHour = nCurrentPulseHour;

                        SaveProfile(g_nCurrentProfile);
                      }
                    }
                  }
                  break;
              }
            }
          }
        }
      }

      if (!bApplyIrrigation && // Si no se está aplicando un Pulso de Riego
          bIsTheLastPulse &&  // Si es/fue el último Pulso de Riego
          digitalRead(RELAYS_MAP[LIGHTS].Pin)) {  // El relé se apagó (Completo el fotoperiodo)
        // Ahora si, incrementamos el contador de "Días"
        g_nIrrigationDayCounter++;

        g_nLastWateredHour = -1;

        SaveProfile(g_nCurrentProfile);

        bIsTheLastPulse = false;

        if (g_nIrrigationSolutionLevel <= 25) {
          char cBuffer[41];
          snprintf(cBuffer, sizeof(cBuffer), "Reservorio de Solución de Riego al %d%.", g_nIrrigationSolutionLevel);
          SendNotification(cBuffer);
        }
      }
    }
    // ================================================== Fans Section ================================================== //
    {
      static uint64_t nFansRestIntervalTimer = 0;

      if (g_nFansRestTimeStartedAt == 0 && (nCurrentMillis - nFansRestIntervalTimer) >= g_nFansRestInterval) {  // If are not in rest time and, the last rest was "g_nFansRestInterval" before, start rest time
        nFansRestIntervalTimer = nCurrentMillis;
        g_nFansRestTimeStartedAt = nCurrentMillis;

        LOGGER(INFO, true, "Fans Rest mode Started.");
      } else {
        if (g_nFansRestTimeStartedAt > 0 && (nCurrentMillis - g_nFansRestTimeStartedAt) >= g_nFansRestDuration) { // If rest time was transcurred, stop rest time
          nFansRestIntervalTimer = nCurrentMillis;
          g_nFansRestTimeStartedAt = 0;

          LOGGER(INFO, true, "Fans Rest time Completed.");
        }
      }

#ifdef USE_DHT11
      bool bGeneralTurnOn = g_nEnvironmentTemperature > 0 && g_nEnvironmentHumidity > 0;
#elif defined(USE_DHT22)
      bool bGeneralTurnOn = g_fEnvironmentTemperature > 0.0f && g_fEnvironmentHumidity > 0.0f;
#endif
      // =============== Internal Fan control by Temperature =============== //
      static bool bInternalFanByTemperature = false;

#ifdef USE_DHT11
      if (g_nEnvironmentTemperature >= g_nStartInternalFanTemperature)
        bInternalFanByTemperature = true;
      else if (g_nEnvironmentTemperature <= (g_nStartInternalFanTemperature - g_nTemperatureStopHysteresis))
        bInternalFanByTemperature = false;
#elif defined(USE_DHT22)
      if (g_fEnvironmentTemperature >= g_nStartInternalFanTemperature)
        bInternalFanByTemperature = true;
      else if (g_fEnvironmentTemperature <= (g_nStartInternalFanTemperature - g_nTemperatureStopHysteresis))
        bInternalFanByTemperature = false;
#endif

      bool bInternalFan = g_nInternalFanMode == 2 || (bGeneralTurnOn && g_nFansRestTimeStartedAt == 0 && g_nInternalFanMode == 1 && bInternalFanByTemperature);
      if (bInternalFan) {
        if (digitalRead(RELAYS_MAP[INTERNAL_FAN].Pin)) {
          digitalWrite(RELAYS_MAP[INTERNAL_FAN].Pin, RELAY_PIN_ON);

          LOGGER(INFO, true, "%s Started.", RELAYS_MAP[INTERNAL_FAN].Name);
        }
      } else {
        if (!digitalRead(RELAYS_MAP[INTERNAL_FAN].Pin)) {
          digitalWrite(RELAYS_MAP[INTERNAL_FAN].Pin, RELAY_PIN_OFF);

          LOGGER(INFO, true, "%s Stopped.", RELAYS_MAP[INTERNAL_FAN].Name);
        }
      }
      // =============== Ventilation Fans control by Temperature & Humidity =============== //
      static bool bVentilationByTemperature = false;
      static bool bVentilationByHumidity = false;

#ifdef USE_DHT11
      if (g_nEnvironmentTemperature >= g_nStartVentilationTemperature)
        bVentilationByTemperature = true;
      else if (g_nEnvironmentTemperature <= (g_nStartVentilationTemperature - g_nTemperatureStopHysteresis))
        bVentilationByTemperature = false;

      if (g_nEnvironmentHumidity >= g_nStartVentilationHumidity)
        bVentilationByHumidity = true;
      else if (g_nEnvironmentHumidity <= (g_nStartVentilationHumidity - g_nHumidityStopHysteresis))
        bVentilationByHumidity = false;
#elif defined(USE_DHT22)
      if (g_fEnvironmentTemperature >= g_nStartVentilationTemperature)
        bVentilationByTemperature = true;
      else if (g_fEnvironmentTemperature <= (g_nStartVentilationTemperature - g_nTemperatureStopHysteresis))
        bVentilationByTemperature = false;

      if (g_fEnvironmentHumidity >= g_nStartVentilationHumidity)
        bVentilationByHumidity = true;
      else if (g_fEnvironmentHumidity <= (g_nStartVentilationHumidity - g_nHumidityStopHysteresis))
        bVentilationByHumidity = false;
#endif

      bool bVentilationFans = g_nVentilationMode == 2 || (bGeneralTurnOn && g_nFansRestTimeStartedAt == 0 && g_nVentilationMode == 1 && (bVentilationByTemperature || bVentilationByHumidity));
      if (bVentilationFans) {
        if (digitalRead(RELAYS_MAP[VENTILATION_FANS].Pin)) {
          digitalWrite(RELAYS_MAP[VENTILATION_FANS].Pin, RELAY_PIN_ON);

          LOGGER(INFO, true, "%s Started.", RELAYS_MAP[VENTILATION_FANS].Name);
        }
      } else {
        if (!digitalRead(RELAYS_MAP[VENTILATION_FANS].Pin)) {
          digitalWrite(RELAYS_MAP[VENTILATION_FANS].Pin, RELAY_PIN_OFF);

          LOGGER(INFO, true, "%s Stopped.", RELAYS_MAP[VENTILATION_FANS].Name);
        }
      }
    }
    // ================================================== Pumps Flow Test Section ================================================== //
    if (g_nTestPumpID != 0) {
      if (PowerSupplyControl(true)) {
        static uint64_t nTestPumpTimer = 0;

        if (nTestPumpTimer == 0) {
          nTestPumpTimer = nCurrentMillis;
        } else {
          static bool bPowerStabilized = false;

          if (!bPowerStabilized && (nCurrentMillis - nTestPumpTimer) >= SECONDS_WAIT_AFTER_TURN_ON_POWER_SUPPLY) {
            bPowerStabilized = true;
          } else if (bPowerStabilized) {
            if (digitalRead(RELAYS_MAP[g_nTestPumpID].Pin)) {
              digitalWrite(RELAYS_MAP[g_nTestPumpID].Pin, RELAY_PIN_ON);

              nTestPumpTimer = nCurrentMillis;

              LOGGER(INFO, true, "%s Flow test Started.", RELAYS_MAP[g_nTestPumpID].Name);
            } else {
              if (nTestPumpTimer > 0 && (nCurrentMillis - nTestPumpTimer) >= FLOW_TEST_DURATION) {
                digitalWrite(RELAYS_MAP[g_nTestPumpID].Pin, RELAY_PIN_OFF);

                LOGGER(INFO, true, "%s Flow test Finished.", RELAYS_MAP[g_nTestPumpID].Name);

                PowerSupplyControl(false);

                bPowerStabilized = false;
                nTestPumpTimer = 0;
                g_nTestPumpID = 0;
              }
            }
          }
        }
      }
    }
    // ================================================== Manual Mixing Pump Activation Section ================================================== //
    if (g_bManualMixing) {
      if (PowerSupplyControl(true)) {
        static uint64_t nMixingTimer = 0;

        if (nMixingTimer == 0) {
          nMixingTimer = nCurrentMillis;
        } else {
          static bool bPowerStabilized = false;

          if (!bPowerStabilized && (nCurrentMillis - nMixingTimer) >= SECONDS_WAIT_AFTER_TURN_ON_POWER_SUPPLY) {
            bPowerStabilized = true;
          } else if (bPowerStabilized) {
            if (digitalRead(RELAYS_MAP[MIXING_PUMP].Pin)) {
              digitalWrite(RELAYS_MAP[MIXING_PUMP].Pin, RELAY_PIN_ON);

              nMixingTimer = nCurrentMillis;

              LOGGER(INFO, true, "Manual Mixing Started.");
            } else {
              if ((nCurrentMillis - nMixingTimer) >= g_nMixingPumpDuration /*Ticks*/) {
                digitalWrite(RELAYS_MAP[MIXING_PUMP].Pin, RELAY_PIN_OFF);

                LOGGER(INFO, true, "Manual Mixing Stopped.");

                PowerSupplyControl(false);

                bPowerStabilized = false;
                nMixingTimer = 0;
                g_bManualMixing = false;
              }
            }
          }
        }
      }
    }
    // ================================================== Store Data for Graph Section ================================================== //
    {
      static uint64_t nLastStoreElapsedTime = 0;

      if ((nCurrentMillis - nLastStoreElapsedTime) >= g_nSamplingInterval) {
        nLastStoreElapsedTime = nCurrentMillis;

#ifdef USE_DHT11
        String strValues = String(pTimeNow) + "|" + String(g_nEnvironmentTemperature) + "|" + String(g_nEnvironmentHumidity);
#elif defined(USE_DHT22)
        String strValues = String(pTimeNow) + "|" + String(g_fEnvironmentTemperature, 1) + "|" + String(g_fEnvironmentHumidity, 1);
#endif

        for (uint8_t i = 0; i < nSoilMoisturePinsCount; i++) {
          g_nSoilsHumidity[i] = GetSoilHumidity(i);

          strValues += "|" + String(g_nSoilsHumidity[i]);
        }

        strValues += "|" + String((digitalRead(RELAYS_MAP[LIGHTS].Pin) || (g_nLightBrightness < 401)) ? 0 // If digitalRead == true Or g_nLightBrightness is less than 401 equals to Relay OFF so: 0  // WARNING: Hardcode offset
                                                                                                                                        : g_nLightBrightness); // If not is OFF, return the Brightness Level

        char cBuffer[29];
        snprintf(cBuffer, sizeof(cBuffer), "/metrics/metrics_%02d_%04d.txt", currentTime.tm_mon + 1, currentTime.tm_year + 1900);

        WriteToSD(cBuffer, strValues.c_str(), true);

        for (int8_t i = 0; i < (MAX_GRAPH_MARKS - 1); i++) {
          strncpy(g_strArrayGraphData[i], g_strArrayGraphData[i + 1], MAX_GRAPH_MARKS_LENGTH - 1);
          g_strArrayGraphData[i][MAX_GRAPH_MARKS_LENGTH - 1] = '\0';
        }

        strncpy(g_strArrayGraphData[MAX_GRAPH_MARKS - 1], strValues.c_str(), MAX_GRAPH_MARKS_LENGTH - 1);
        g_strArrayGraphData[MAX_GRAPH_MARKS - 1][MAX_GRAPH_MARKS_LENGTH - 1] = '\0';
      }
    }
  }

#if defined(TEST_MODE) && defined(ENABLE_SERIAL_LOGGER)
  size_t freeHeap = ESP.getFreeHeap();

  Serial.printf("[INFO] Free Heap: %u bytes after all loop function execute.\n", freeHeap);
#endif
}
