#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP_EEPROM.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>


#include "config.h"
#include "index.h"

#define DEBUG

#ifdef DEBUG
#  define debug_printf(...) Serial.printf(__VA_ARGS__)
#  define debug_println(x) Serial.println(x)
#else
#  define debug_printf(...) do {} while (0)
#  define debug_println(x) do {} while (0)
#endif

WiFiClient espClient;
PubSubClient client(espClient);

const long reconnect_delay = 10000;

long lastWifiConnectAttempt = 0;
long lastMQTTConnectAttempt = 0;

int drainDuration = 500; // 0.5 seconds
unsigned long lastMistTime = 0; // time of last misting in millis since start
int mistStartSeconds = 0; // time of last misting in epoch seconds; 0 means nothing has happened

bool pumpOn = false;
bool pumpOverride = false; // To force pump on for draining

float pressure;

ESP8266WebServer server(80);

// =============================
// Pin definitions
// =============================
const int solenoidRelay = D4;
const int switchPin = D6;

const int pumpRelay = D7;
const int pressureSensor = A0;

const int mistSolenoid = D1;  // GPIO5
const int drainSolenoid = D2; // GPIO4
// GPIO5, GPIO4 only pins low on boot
// https://community.blynk.cc/t/esp8266-gpio-pins-info-restrictions-and-features/22872

// =============================
// NTP
// =============================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -7*3600, 60000);

// =============================
// MQTT topics
// =============================
// sensors
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
#define mqttMistersOn "aero/mistersOn"
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
    client.publish(mqttErrors, errorMessage.c_str());
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
// Callbacks
// =============================
void updateMistInterval(int mistMillis) {
  if (mistMillis < 500) {
    String errorMessage = "Mist interval should be >= 500 ms";
    client.publish(mqttErrors, errorMessage.c_str());
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

void updateMistDuration(int mistMillis) {
  if (mistMillis < 500) {
    String errorMessage = "Mist duration should be >= 500 ms";
    client.publish(mqttErrors, errorMessage.c_str());
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

void updateLastMistTime(long receivedMistTime) {
  debug_printf("Received mist time of %d, mistStartSeconds=%d\n", receivedMistTime, mistStartSeconds);
  if (receivedMistTime > mistStartSeconds) {
    mistStartSeconds = receivedMistTime;
    // Set the lastMistTime millis based on the time since the last recorded misting
    int millisSinceMisting = (timeClient.getEpochTime() - mistStartSeconds) * 1000;
    if (millisSinceMisting > 0) {
      lastMistTime = millis() - millisSinceMisting;
      debug_printf("Setting last mist time millis to %d\n", lastMistTime);
    } else { // something went wrong getting current time
      mistStartSeconds = -1;
      String errorMessage = 
        "Not setting last mist time: it appears to be in the future. Time now: " 
          + String(timeClient.getEpochTime());
      client.publish(mqttErrors, errorMessage.c_str());
      debug_println(errorMessage);
    }
  } else {
    mistStartSeconds = -1;
    String errorMessage = "Not setting last mist time: invalid value";
    client.publish(mqttErrors, errorMessage.c_str());
    debug_println(errorMessage);
  }
}

void updateMistingEnabled(const char* payload) {
  if (payload[0] == '1') {
    settings.misting_enabled = true;
  } else {
    settings.misting_enabled = false;
  }
  saveConfig();
}

void updatePumpEnabled(const char* payload) {
  if (payload[0] == '1') {
    settings.pump_enabled = true;
  } else {
    settings.pump_enabled = false;
  }
  saveConfig();
}

void updateMinPSI(int PSI) {
  if (PSI < 10) {
    String errorMessage = "Min PSI should be >= 10";
    client.publish(mqttErrors, errorMessage.c_str());
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

void updateMaxPSI(int PSI) {
  if (PSI > 115) {
    String errorMessage = "Max PSI should be <= 115";
    client.publish(mqttErrors, errorMessage.c_str());
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
// WiFi / MQTT setup
// =============================
WiFiEventHandler gotIpEventHandler;

void reconnectWiFi() {
  Serial.printf("Connecting to %s ...\n", SSID);
  WiFi.begin(SSID, WIFI_PWD);
}

#define MQTT_SERVER "192.168.0.134"
#define MQTT_CLIENT "AeroClient"
#define WILL_TOPIC "aero/status"
#define WILL_MESSAGE "DISCONNECTED"
const int willQoS = 0;

void mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
  char message[length+1];
  for (int i=0; i<length; i++) {
    message[i] = (char)payload[i];
  }
  message[length] = NULL;
  
  if (strcmp(topic, mqttPiLastMistTime) == 0) {
    updateLastMistTime(atoi(message));
    return;
  }
  if (strcmp(topic, mqttCommandEnableMisting) == 0) {
    updateMistingEnabled(message);
    return;
  }
  if (strcmp(topic, mqttCommandSetMistIntervalMillis) == 0) {
    updateMistInterval(atoi(message));
    return;
  }
  if (strcmp(topic, mqttCommandSetMistDurationMillis) == 0) {
    updateMistDuration(atoi(message));
    return;
  }
  if (strcmp(topic, mqttCommandSetMinPSI) == 0) {
    updateMinPSI(atoi(message));
    return;
  }
  if (strcmp(topic, mqttCommandSetMaxPSI) == 0) {
    updateMaxPSI(atoi(message));
    return;
  }
  debug_printf("Got topic %s that is not handled!\n", topic);
  client.publish(mqttErrors, "Got topic that is not handled");
}

// Reconnect
boolean reconnectMQTT() {
  if (client.connect(MQTT_CLIENT, MQTT_USERNAME, MQTT_PASSWD, WILL_TOPIC, true, willQoS, WILL_MESSAGE)) {
    client.subscribe(mqttPiLastMistTime);
    client.subscribe(mqttCommandEnableMisting);
    client.subscribe(mqttCommandSetMistIntervalMillis);
    client.subscribe(mqttCommandSetMistDurationMillis);
    client.subscribe(mqttCommandSetMinPSI);
    client.subscribe(mqttCommandSetMaxPSI);
  
    // Publish a message to indicate connection
    client.publish("aero/status", "CONNECTED", true);
    client.publish(mqttPumpStatus, pumpOn ? "on" : "off", true);
    publishConfig();
  }
  return client.connected();
}

// =============================
// Misting state
// =============================
enum MistingState { none, waiting };
MistingState mistingState = waiting; // start in waiting in case pressure is low

// Blocking method to perform the mist cycle uninterrupted.
void mist() {
  digitalWrite(mistSolenoid, HIGH);
  digitalWrite(drainSolenoid, LOW);
  
  delay(settings.mist_duration_millis);
  
  debug_println("Stopping misting");
  debug_println("Starting draining");
  digitalWrite(mistSolenoid, LOW);
  digitalWrite(drainSolenoid, HIGH);

  delay(drainDuration);
  
  debug_println("Stopping draining");
  digitalWrite(drainSolenoid, LOW);
  digitalWrite(mistSolenoid, LOW);
  mistingState = none;
}

static void updateSolenoids() {
  static unsigned long drainStart = 0;

  if (!settings.misting_enabled) {
    digitalWrite(mistSolenoid, LOW);
    digitalWrite(drainSolenoid, LOW);
    return;
  }

  if (mistingState == none && millis() - lastMistTime > settings.mist_interval_millis) {
    lastMistTime = millis();
    mistStartSeconds = timeClient.getEpochTime();
    
    logMistingStart();

    mist();

    logMistingStop();

    return;
  }
}

// Send the time of the last misting and the time of the next scheduled misting
void logMistingStart() {
  client.publish(mqttLastMistTime, String(mistStartSeconds).c_str());
  client.publish(mqttNextMistTime, String(mistStartSeconds + settings.mist_interval_millis / 1000.0).c_str());
  client.publish(mqttMistersOn, "1");
}

void logMistingStop() {
  client.publish(mqttMistersOn, "0");
}

// =============================
// Pressure
// =============================
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
}

void logPressure(float pressure) {
  client.publish(mqttPressure, String(pressure).c_str(), false);
}

void updatePump(float pressure) {
  if (!settings.pump_enabled) {
    if (pumpOn) {
      pumpOn = false;
      digitalWrite(pumpRelay, LOW);
      logPumpStatus();
    }
    return;
  }
  if (pumpOverride) {
    if (!pumpOn) {
      pumpOn = true;
      digitalWrite(pumpRelay, HIGH);
      logPumpStatus();
    }
    return;
  }
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
}

// =============================
// OTA
// =============================
void setupOTA() {
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
}

// =============================
// Server
// =============================
#define MIST_DURATION_ID "MistDuration"
#define MIST_INTERVAL_ID "MistInterval"
#define MIN_PSI_ID "MinPSI"
#define MAX_PSI_ID "MaxPSI"
#define MIST_STATUS_ID "MistStatus"
#define PUMP_STATUS_ID "PumpStatus"
#define PRESSURE_ID "Pressure"
#define PUMP_OVERRIDE_ID "PumpOverride"

void handleRoot() {
  String s = MAIN_page; //Read HTML contents
  server.send(200, "text/html", s); //Send web page
}

void handleSetValue() {
  String value = server.arg("value");
  String field = server.arg("field");
  
  Serial.println(value);
  if (field.equals(MIST_DURATION_ID)) {
    updateMistDuration(value.toInt());
  } else if (field.equals(MIST_INTERVAL_ID)) {
    updateMistInterval(value.toInt());
  } else if (field.equals(MIN_PSI_ID)) {
    updateMinPSI(value.toInt());
  } else if (field.equals(MAX_PSI_ID)) {
    updateMaxPSI(value.toInt());
  } else if (field.equals(MIST_STATUS_ID)) {
    updateMistingEnabled(value.c_str());
  } else if (field.equals(PUMP_STATUS_ID)) {
    updatePumpEnabled(value.c_str());
  } else if (field.equals(PUMP_OVERRIDE_ID)) {
    pumpOverride = value.equals("1") ? true : false;
  } else {
    debug_printf("Received unsupported field from server: %s\n", field.c_str());
    return;
  }
  
  server.send(200, "text/plain", value);
}

void handleGetValue() {
  String field = server.arg("field");
  String value;
  
  if (field.equals(MIST_DURATION_ID)) {
    value = String(settings.mist_duration_millis);
  } else if (field.equals(MIST_INTERVAL_ID)) {
    value = String(settings.mist_interval_millis);
  } else if (field.equals(MIN_PSI_ID)) {
    value = String(settings.pump_min_pressure);
  } else if (field.equals(MAX_PSI_ID)) {
    value = String(settings.pump_max_pressure);
  } else if (field.equals(MIST_STATUS_ID)) {
    value = String(settings.misting_enabled);
  } else if (field.equals(PUMP_STATUS_ID)) {
    value = String(settings.pump_enabled);
  } else if (field.equals(PUMP_OVERRIDE_ID)) {
    value = String(pumpOverride);
  } else if (field.equals(PRESSURE_ID)) {
    value = String(pressure);
  } else {
    debug_printf("Received unsupported field from server: %s\n", field.c_str());
    return;
  }
  
  server.send(200, "text/plain", value);
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

  gotIpEventHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& event) {
    Serial.print("Connected to WiFi, IP: ");
    Serial.println(WiFi.localIP());
  });

  client.setServer(MQTT_SERVER, 1883);
  client.setCallback(mqttMessageCallback);

  reconnectWiFi();
  // Wait up to 5 seconds for initial wifi connection
  int counter = 0;
  lastWifiConnectAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && counter++ < 5) { // Wait for the Wi-Fi to connect
    delay(1000);
  }

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.begin();
    timeClient.update();
    Serial.println(timeClient.getFormattedTime());
    
    setupOTA();

    // Attempt up to 3 times to initially connect to MQTT broker
    int attempts = 0;
    lastMQTTConnectAttempt = millis();
    while (!client.connected() && attempts++ < 3) {
      debug_println("Attempting to connect to MQTT");
      if (reconnectMQTT()) {
        break;
      }
      delay(3000); // wait a few seconds before trying again
    }
    if (client.connected()) {
      debug_println("Connected to MQTT");
    } else {
      debug_println("Failed to connect to MQTT");
      // Don't wait for last mist time from Pi
      mistStartSeconds = -1;
    }
  }

  EEPROM.begin(sizeof(config_type));
  bool ok = loadConfig();
  Serial.printf("Loaded config,%s from storage\n", ok ? "" : " not");

  publishConfig();

  server.on("/", handleRoot);
  server.on("/setValue", handleSetValue);
  server.on("/getValue", handleGetValue);

  server.begin();
}

void loop() {
  ArduinoOTA.handle();
  client.loop();
  server.handleClient();
  timeClient.update();

  // Wait a short time after startup to receive last mist time from pi.
  // Otherwise, act like we haver never misted.
  if (mistStartSeconds == 0) {
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

  // Handle reconnects
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiConnectAttempt > reconnect_delay) {
    lastWifiConnectAttempt = millis();
    debug_println("Attempting to reconnect to WiFi");
    reconnectWiFi();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected() && millis() - lastMQTTConnectAttempt > reconnect_delay) {
      lastMQTTConnectAttempt = millis();
      debug_println("Attempting to reconnect to MQTT");
      reconnectMQTT();
    }
  }

  if (measurePressure(&pressure) == true) {
    debug_printf("pressure reading: %f\n", pressure);
    logPressure(pressure);
    
    updatePump(pressure);

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
}
