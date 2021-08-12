#include <ArduinoOTA.h>
#include <dht_nonblocking.h>
#include "EspMQTTClient.h"
#include <ESP_EEPROM.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>

#include "config.h"

#define DEBUG

#ifdef DEBUG
#  define debug_printf(...) Serial.printf(__VA_ARGS__)
#  define debug_println(x) Serial.println(x)
#else
#  define debug_printf(...) do {} while (0)
#  define debug_println(x) do {} while (0)
#endif

// =============================
// Pin definitions
// =============================
const int dhtPin = D5;
const int solenoidRelay = D4;
const int switchPin = D6;

const int pumpRelay = D7;
const int pressureSensor = A0;

const int mistSolenoid = D1;  // GPIO5
const int drainSolenoid = D2; // GPIO4
// GPIO5, GPIO4 only pins low on boot
// https://community.blynk.cc/t/esp8266-gpio-pins-info-restrictions-and-features/22872

// =============================
// MQTT setup
// =============================
EspMQTTClient client(
  SSID,
  WIFI_PWD,
  "192.168.0.134",  // MQTT Broker server ip
 // "MQTTUsername",   // Can be omitted if not needed
 // "MQTTPassword",   // Can be omitted if not needed
  "AeroClient",     // Client name that uniquely identify your device
  1883              // The MQTT port, default to 1883. this line can be omitted
);

// sensors
#define mqttTemp1 "aero/temp1"
#define mqttHumidity1 "aero/humidity1"
#define mqttPressure "aero/pressure"

#define mqttPumpStatus "aero/pump"

// config
#define mqttMistEnabled "aero/config/mistingEnabled"
#define mqttMistDuration "aero/config/mistDuration"
#define mqttMistInterval "aero/config/mistInterval"
#define mqttPumpEnabled "aero/config/pumpEnabled"
#define mqttPumpMinPSI "aero/config/pumpMinPSI"
#define mqttPumpMaxPSI "aero/config/pumpMaxPSI"

// commands
#define mqttCommandEnableMisting "aerocommand/setMistingEnabled"
#define mqttCommandSetMistDurationMillis "aerocommand/setMistDurationMillis"
#define mqttCommandSetMistIntervalMillis "aerocommand/setMistIntervalMillis"
#define mqttCommandEnablePump "aerocommand/setPumpEnabled"
#define mqttCommandSetMinPSI "aerocommand/setMinPSI"
#define mqttCommandSetMaxPSI "aerocommand/setMaxPSI"

// misting
#define mqttLastMistTime "aero/lastMistTime"
#define mqttNextMistTime "aero/nextMistTime"
#define mqttPiLastMistTime "aeroPi/lastMistTime" // sent from Pi on connection established

#define mqttErrors "aero/error"


// =============================
// Configuration
// =============================
#define CONFIG_VERSION "a01"

// configuration to be stored in EEPROM
struct config_type
{
  char config_version[4];
  bool misting_enabled;
  int mist_duration_millis;
  int mist_interval_millis;
  bool pump_enabled;
  int pump_min_pressure;
  int pump_max_pressure;
};

config_type settings;

void setDefaultConfig() {
  sprintf(settings.config_version, "%s", CONFIG_VERSION);
  settings.misting_enabled = true;
  settings.mist_duration_millis = 3000; // 3 seconds
  settings.mist_interval_millis = 5 * 60 * 1000; // 5 minutes
  settings.pump_enabled = true;
  settings.pump_min_pressure = 80;
  settings.pump_max_pressure = 100;
}


// Attempt to load config from EEPROM
bool loadConfig() {
  EEPROM.get(0, settings);
  if (strcmp(CONFIG_VERSION, settings.config_version) != 0) {
    // config invalid; revert to default
    setDefaultConfig();
    String errorMessage = "Failed to load config; reverting to default";
    client.publish(mqttErrors, errorMessage);
    debug_println(errorMessage);
    return false;
  }
  debug_printf("Loaded config %s from EEPROM\n", settings.config_version);
  return true;

}

// save the config to flash
void saveConfig() {
  // set the EEPROM data ready for writing
  EEPROM.put(0, settings);

  // commit (write) the data to EEPROM - only actually writes if there has been a change
  bool ok = EEPROM.commit();
  Serial.println((ok) ? "Config commit OK" : "Config commit failed");
  publishConfig();
}

void publishConfig() {
  client.publish(mqttMistEnabled, String(settings.misting_enabled).c_str(), true);
  client.publish(mqttMistDuration, String(settings.mist_duration_millis).c_str(), true);
  client.publish(mqttMistInterval, String(settings.mist_interval_millis).c_str(), true);
  client.publish(mqttPumpEnabled, String(settings.pump_enabled).c_str(), true);
  client.publish(mqttPumpMinPSI, String(settings.pump_min_pressure).c_str(), true);
  client.publish(mqttPumpMaxPSI, String(settings.pump_max_pressure).c_str(), true);
}


// =============================
// Misting state
// =============================
enum MistingState { none, mist, bleed, full_drain, waiting };
MistingState mistingState = waiting; // start in waiting in case pressure is low
int bleedDuration = 500; // 0.5 seconds
unsigned long lastMistTime = 0; // time of last misting in millis since start
int mistStartSeconds = 0; // time of last misting in epoch seconds; 0 means nothing has happened

static void updateSolenoids() {
  static unsigned long bleedStart = 0;
  
  static unsigned long printTime = 0;

  if (millis() - printTime > 500) {
    Serial.printf("mistingState = %d\n", mistingState);
    printTime = millis();
  }

  if (!settings.misting_enabled) {
    digitalWrite(mistSolenoid, LOW);
    digitalWrite(drainSolenoid, LOW);
    return;
  }

  if (mistingState == none && millis() - lastMistTime > settings.mist_interval_millis) {
    digitalWrite(mistSolenoid, HIGH);
    digitalWrite(drainSolenoid, LOW);
    lastMistTime = millis();
    mistingState = mist;
    mistStartSeconds = time(nullptr);
    logMisting();
    return;
  }
  if (mistingState == mist && millis() - lastMistTime > settings.mist_duration_millis) {
    // Stop misting, start draining
    debug_printf("Stopping misting after %d millis\n", millis() - lastMistTime);
    debug_println("Starting bleeding");
    digitalWrite(mistSolenoid, LOW);
    digitalWrite(drainSolenoid, HIGH);
    bleedStart = millis();
    mistingState = bleed;
    return;
  }
  if (mistingState == bleed && millis() - bleedStart > bleedDuration) {
    // Stop draining
    debug_printf("Stopping bleeding after %d millis\n", millis() - bleedStart);
    digitalWrite(drainSolenoid, LOW);
    digitalWrite(mistSolenoid, LOW);
    mistingState = none;
    return;
  }
}

// Send the time of the last misting and the time of the next scheduled misting
void logMisting() {
  client.publish(mqttLastMistTime, String(mistStartSeconds).c_str());
  client.publish(mqttNextMistTime, String(mistStartSeconds + settings.mist_interval_millis / 1000.0).c_str());
}


// =============================
// InfluxDB
// =============================

// Set timezone string according to https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
#define TZ_INFO "PST8PDT"

// InfluxDB client instance with preconfigured InfluxCloud certificate
InfluxDBClient influxClient(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);

// Data point
Point ambientPoint("ambient");
Point pressurePoint("pressure");
Point pumpStatusPoint("pump");

void writeToInfluxDB(Point point) {
  if (mistingState == mist || mistingState == bleed) {
    // Writing is too slow to do during precise time events
    debug_println("Skipping InfluxDB write due to misting");
    return;
  }
  debug_printf("Writing: %s\n", point.toLineProtocol().c_str());

  if (!influxClient.writePoint(point)) {
    debug_printf("InfluxDB write failed: %s\n", influxClient.getLastErrorMessage().c_str());
    client.publish(mqttErrors, influxClient.getLastErrorMessage().c_str());
    }
}

// =============================
// Temperature and Humidity setup
// =============================
#define DHT_SENSOR_TYPE DHT_TYPE_11

DHT_nonblocking dhtSensor(dhtPin, DHT_SENSOR_TYPE);
const int temperatureInterval = 30000; // 30 seconds between readings

static bool measureTempAndHumidity(float *temperature, float *humidity) {
  static unsigned long measurementTimestamp = millis();

  if (millis() - measurementTimestamp > temperatureInterval)
  {
    if (dhtSensor.measure(temperature, humidity))
    {
      measurementTimestamp = millis();
      return(true);
    }
  }

  return(false);
}

void logTempAndHumidity(float temperature, float humidity) {
  debug_printf("T = %.1f deg. C (%.1f deg. F), H = %.1f%%\n",
      temperature,
      temperature * 9.0 / 5 + 32,
      humidity);
  client.publish(mqttTemp1, String(temperature).c_str(), true);
  client.publish(mqttHumidity1, String(humidity).c_str(), true);

  ambientPoint.clearFields();

  ambientPoint.addField("temperature", temperature);
  ambientPoint.addField("humidity", humidity);

  writeToInfluxDB(ambientPoint);
}

// =============================
// Pressure
// =============================
bool pumpOn = false;
// Don't change state unless we have two readings in a row outside range
bool lastPressureReadingLow = false;
bool lastPressureReadingHigh = false;

// Convert the analog pin reading to PSI
float analogToPSI(const float& analogReading) {
  // Determined through experiment / line fitting to match analog gauge
  float psi = 0.188 * analogReading - 17.8;

  return(psi);
}

static bool measurePressure(float *pressure) {
  static unsigned long measurement_timestamp = millis();

  if (mistingState == mist) {
    // Don't do anything while misting.
    // It doesn't actually seem to affect the readings currently,
    // but I'm not sure it won't, and we anyway we don't want to
    // do anything in response to pressure reading this while misting.
    return false;
  }

  float averageReading = 0;
  if (millis() - measurement_timestamp > 5000) {
    for (int i = 0; i < 5; i++) {
      float tempReading = analogRead(pressureSensor);
      averageReading += tempReading;
    }
    measurement_timestamp = millis();
    *pressure = analogToPSI(averageReading / 5);
    return(true);
  }
  return(false);
}

void logPumpStatus() {
  client.publish(mqttPumpStatus, pumpOn ? "on" : "off", true);

  pumpStatusPoint.clearFields();
  pumpStatusPoint.addField("status", pumpOn ? 1 : 0);
  
  writeToInfluxDB(pumpStatusPoint);
}

void logPressure(float pressure) {
  client.publish(mqttPressure, String(pressure).c_str(), true);

  pressurePoint.clearFields();
  pressurePoint.addField("psi", pressure);

  writeToInfluxDB(pressurePoint);
}


// Callbacks
void updateMistInterval(const String& payload) {
  int mistMillis = payload.toInt();
  if (mistMillis < 500) {
    String errorMessage = "Mist interval should be >= 500 ms";
    client.publish(mqttErrors, errorMessage);
    debug_println(errorMessage);
  } else if (mistMillis <= settings.mist_duration_millis) {
    client.publish(mqttErrors, "Mist interval should be greater than mist duration");
    debug_printf(
      "Mist interval should be greater than mist duration: got %d, current mist duration is %d\n",
      mistMillis,
      settings.mist_duration_millis);
  } else {
    settings.mist_interval_millis = mistMillis;
    debug_printf("Setting mist interval to %d ms\n", mistMillis);
  }
  saveConfig();
  // update next misting time
  client.publish(mqttNextMistTime, String(mistStartSeconds + settings.mist_interval_millis / 1000.0).c_str());
}

void updateMistDuration(const String& payload) {
  int mistMillis = payload.toInt();
  if (mistMillis < 500) {
    String errorMessage = "Mist duration should be >= 500 ms";
    client.publish(mqttErrors, errorMessage);
    debug_println(errorMessage);
  } else if (mistMillis > settings.mist_interval_millis) {
    client.publish(mqttErrors, "Mist duration must be less than mist interval");
    debug_printf(
      "Mist duration must be less than mist interval (got %d, current mist interval is %d)\n",
      mistMillis,
      settings.mist_interval_millis);
  } else {
    settings.mist_duration_millis = mistMillis;
    debug_printf("Setting mist duration to %d ms\n", mistMillis);
  }
  saveConfig();
}

void updateLastMistTime(const String& payload) {
  int receivedMistTime = payload.toInt();
  if (receivedMistTime > mistStartSeconds) {
    mistStartSeconds = receivedMistTime;
    // Set the lastMistTime millis based on the time since the last recorded misting
    int millisSinceMisting = (time(nullptr) - mistStartSeconds) * 1000;
    if (millisSinceMisting > 0) {
      lastMistTime = millis() - millisSinceMisting;
      debug_printf("Setting last mist time millis to %d\n", lastMistTime);
    } else { // something went wrong getting current time
      mistStartSeconds = -1;
      String errorMessage = 
        "Not setting last mist time: it appears to be in the future. Time now: " 
          + String(time(nullptr));
      client.publish(mqttErrors, errorMessage);
      debug_println(errorMessage);
    }
  } else {
    mistStartSeconds = -1;
    String errorMessage = "Not setting last mist time: invalid value";
    client.publish(mqttErrors, errorMessage);
    debug_println(errorMessage);
  }
}

void updateMistingEnabled(const String& payload) {
  if (payload == "1") {
    settings.misting_enabled = true;
  } else {
    settings.misting_enabled = false;
  }
  saveConfig();
}

void updateMinPSI(const String& payload) {
  int PSI = payload.toInt();
  if (PSI < 10) {
    String errorMessage = "Min PSI should be >= 10";
    client.publish(mqttErrors, errorMessage);
    debug_println(errorMessage);
  } else if (PSI >= settings.pump_max_pressure) {
    client.publish(mqttErrors, "Min PSI must be less than max PSI");
    debug_printf(
      "Min PSI must be less than max PSI (current max PSI is %d)\n",
      settings.pump_max_pressure);
  }else {
    settings.pump_min_pressure = PSI;
    debug_printf("Setting min PSI to %d\n", PSI);
  }
  saveConfig();
}

void updateMaxPSI(const String& payload) {
  int PSI = payload.toInt();
  if (PSI > 115) {
    String errorMessage = "Max PSI should be <= 115";
    client.publish(mqttErrors, errorMessage);
    debug_println(errorMessage);
  } else if (PSI <= settings.pump_min_pressure) {
    client.publish(mqttErrors, "Max PSI must be greater than min PSI");
    debug_printf(
      "Max PSI must be greater than min PSI (current min PSI is %d)\n",
      settings.pump_min_pressure);
  }else {
    settings.pump_max_pressure = PSI;
    debug_printf("Setting max PSI to %d\n", PSI);
  }
  saveConfig();
}

// =============================
// On Connection
// =============================
// This function is called once everything is connected (Wifi and MQTT)
// WARNING : YOU MUST IMPLEMENT IT IF YOU USE EspMQTTClient
void onConnectionEstablished() {
    Serial.println("Wifi connection established!");
  ArduinoOTA.onStart([]() {
    // Make sure we're not misting during upload
    digitalWrite(mistSolenoid, LOW);
    digitalWrite(drainSolenoid, LOW);

    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();

  // Accurate time is necessary for certificate validation and writing in batches
  // For the fastest time sync find NTP servers in your area: https://www.pool.ntp.org/zone/
  // Syncing progress and the time will be printed to Serial.
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

  // Check InfluxDB server connection
  if (influxClient.validateConnection()) {
    debug_printf("Connected to InfluxDB: %s\n", influxClient.getServerUrl());
  } else {
    client.publish(mqttErrors, influxClient.getLastErrorMessage().c_str());
    debug_printf("InfluxDB connection failed: %s\n", influxClient.getLastErrorMessage().c_str());
  }
  
  client.subscribe(mqttPiLastMistTime, updateLastMistTime);

  client.subscribe(mqttCommandEnableMisting, updateMistingEnabled);

  client.subscribe(mqttCommandSetMistIntervalMillis, updateMistInterval);

  client.subscribe(mqttCommandSetMistDurationMillis, updateMistDuration);

  client.subscribe(mqttCommandSetMinPSI, updateMinPSI);

  client.subscribe(mqttCommandSetMaxPSI, updateMaxPSI);

  // Publish a message to indicate connection
  client.publish("aero/status", "CONNECTED", true);
  client.publish(mqttPumpStatus, pumpOn ? "on" : "off", true);
  publishConfig();
}

// =============================
// Setup
// =============================
void setup() {
  pinMode(pumpRelay, OUTPUT);
  pinMode(switchPin, INPUT_PULLUP);
  pinMode(mistSolenoid, OUTPUT);
  pinMode(drainSolenoid, OUTPUT);
  digitalWrite(mistSolenoid, LOW);
  digitalWrite(drainSolenoid, LOW);

  Serial.begin(115200);

  client.enableDebuggingMessages(); // Enable debugging messages sent to serial output
  client.enableLastWillMessage("aero/status", "OFFLINE", true); // Message to be sent in event of disconnection

  EEPROM.begin(sizeof(config_type));
  bool ok = loadConfig();
  Serial.printf("Loaded config,%s from storage\n", ok ? "" : " not");

  // Publish to MQTT here in case we are already connected
  publishConfig();
}

long last_print_millis = 0;

void loop() {
  ArduinoOTA.handle();
  client.loop();

  if (client.getConnectionEstablishedCount() == 0 && millis() < 5000) {
    if (millis() - last_print_millis > 500) {
      Serial.printf("Waiting for connection, millis = %d\n", millis());
      last_print_millis = millis();
    }
    // wait 5 seconds for connections to be established
    return;
  }

  // Wait a short time after startup to receive last mist time from pi.
  // Otherwise, act like we haver never misted.
  if (client.isConnected() && mistStartSeconds == 0) {
    if (millis() - last_print_millis > 500) {
      Serial.printf("Waiting for last mist time, millis = %d\n", millis());
      last_print_millis = millis();
    }
    // means we have never misted and have never received last mist time from pi
    if (millis() < 10000) {
      // wait a few seconds for pi to send last mist time
      return;
    }
    if (mistStartSeconds == 0) {
      debug_println("Giving up waiting for last mist time from pi");
      mistStartSeconds = -1; // give up on waiting
    }
  }

  float pressure;
  if (measurePressure(&pressure) == true) {
    debug_printf("pressure reading: %f\n", pressure);
    logPressure(pressure);
    
    if (pressure > settings.pump_min_pressure) {
      lastPressureReadingLow = false;
    } else if (!pumpOn) {
      if (lastPressureReadingLow) {
        lastPressureReadingLow = false;
        digitalWrite(pumpRelay, HIGH);
        debug_println("turning pump on");
        pumpOn = true;
        logPumpStatus();
      } else {
        lastPressureReadingLow = true;
      }
    }

    if (pressure <= settings.pump_max_pressure) {
      lastPressureReadingHigh = false;
    } else if (pumpOn) {
      if (lastPressureReadingHigh) { // Get two in a row
        lastPressureReadingHigh = false;
        digitalWrite(pumpRelay, LOW);
        pumpOn = false;
        debug_println("turning pump off");
        logPumpStatus();
      } else {
        lastPressureReadingHigh = true;
      }
    }

    if (pressure < settings.pump_min_pressure && settings.misting_enabled && mistingState != waiting) {
      // don't mist when pressure is too low
      mistingState = waiting;
      digitalWrite(mistSolenoid, LOW);
      digitalWrite(drainSolenoid, LOW);
      debug_println("changing mister state to waiting due to low pressure");
    } else if (mistingState == waiting && pressure >= settings.pump_min_pressure && mistStartSeconds != 0) {
      // can start once pressure is high enough and we are not still waiting for last mist time from pi
      mistingState = none;
      debug_println("Pressure high enough; leaving waiting state");
    }
  }

  updateSolenoids();

  float temperature;
  float humidity;

  if(measureTempAndHumidity(&temperature, &humidity) == true ) {
    logTempAndHumidity(temperature, humidity);
  }
}
