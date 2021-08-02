#include <ArduinoOTA.h>
#include <dht_nonblocking.h>
#include "EspMQTTClient.h"
#include <ESP_EEPROM.h>

#define DEBUG

// =============================
// Pin definitions
// =============================
const int DHT_SENSOR_PIN = D5;
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
  "***REMOVED***",
  "***REMOVED***",
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
    Serial.println("Failed to load config; reverting to default");
    return false;
  }
  Serial.printf("Loaded config %s from EEPROM\n", settings.config_version);
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
// Temperature and Humidity setup
// =============================
#define DHT_SENSOR_TYPE DHT_TYPE_11

DHT_nonblocking dht_sensor( DHT_SENSOR_PIN, DHT_SENSOR_TYPE );
const int temperatureInterval = 30000; // 30 seconds between readings

/*
 * Poll for a measurement, keeping the state machine alive.  Returns
 * true if a measurement is available.
 */
static bool measure_environment( float *temperature, float *humidity )
{
  static unsigned long measurement_timestamp = millis( );

  /* Measure once every four seconds. */
  if( millis( ) - measurement_timestamp > temperatureInterval )
  {
    if( dht_sensor.measure( temperature, humidity ) == true )
    {
      measurement_timestamp = millis( );
      return( true );
    }
  }

  return( false );
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
  float psi = analogReading * 0.296 - 25.9;
  Serial.printf("psi = %f\n", psi);
  return(psi);
}

static bool measurePressure( float *pressure ) {
  static unsigned long measurement_timestamp = millis();

  if (millis() - measurement_timestamp > 5000) {
      *pressure = analogToPSI(analogRead(pressureSensor));
      measurement_timestamp = millis();
      return(true);
  }
  return(false);
}


// =============================
// Misting state
// =============================
enum MistingState { none, mist, bleed, full_drain, disabled };
MistingState mistingState = none;
int bleedDuration = 500; // 0.5 seconds


static void updateSolenoids() {
  //static MistingState mistState = none;
  static unsigned long lastMistTime = 0;
  static unsigned long bleedStart = 0;

  if (mistingState == disabled) {
    digitalWrite(mistSolenoid, LOW);
    digitalWrite(drainSolenoid, LOW);
    return;
  }

  if (mistingState == full_drain) {
    digitalWrite(mistSolenoid, HIGH);
    digitalWrite(drainSolenoid, HIGH);
    return;
  }

  if (mistingState == none && millis() - lastMistTime > settings.mist_interval_millis) {
    Serial.println("Starting misting");
    digitalWrite(mistSolenoid, HIGH);
    digitalWrite(drainSolenoid, LOW);
    lastMistTime = millis();
    mistingState = mist;
    return;
  }
  if (mistingState == mist && millis() - lastMistTime > settings.mist_duration_millis) {
    // Stop misting, start draining
    //Serial.println("Starting bleeding");
    digitalWrite(mistSolenoid, LOW);
    digitalWrite(drainSolenoid, HIGH);
    bleedStart = millis();
    mistingState = bleed;
    return;
  }
  if (mistingState == bleed && millis() - bleedStart > bleedDuration) {
    // Stop draining
    //Serial.println("Stopping bleeding");
    digitalWrite(drainSolenoid, LOW);
    digitalWrite(mistSolenoid, LOW);
    mistingState = none;
    return;
  }
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

// =============================
// OTA
// =============================
// This function is called once everything is connected (Wifi and MQTT)
// WARNING : YOU MUST IMPLEMENT IT IF YOU USE EspMQTTClient
void onConnectionEstablished() {
  ArduinoOTA.onStart([]() {
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
  
  // Subscribe to "mytopic/test" and display received message to Serial
//  client.subscribe("mytopic/test", [](const String & payload) {
//    Serial.println(payload);
//  });

  // Subscribe to "mytopic/wildcardtest/#" and display received message to Serial
//  client.subscribe("mytopic/wildcardtest/#", [](const String & topic, const String & payload) {
//    Serial.println("(From wildcard) topic: " + topic + ", payload: " + payload);
//  });

  client.subscribe(mqttCommandEnableMisting, [](const String & payload) {
    Serial.printf("got request to change misting status (%s)", payload);
    if (payload == "1") {
      settings.misting_enabled = true;
    } else {
      settings.misting_enabled = false;
    }
    saveConfig();
  });

  client.subscribe(mqttCommandSetMistIntervalMillis, [](const String & payload) {
    Serial.printf("got request to change misting interval (%s)\n", payload);
    int mistMillis = payload.toInt();
    if (mistMillis < 500) {
      Serial.printf("Mist interval should be >= 500 ms (got %d)\n", mistMillis);
    } else if (mistMillis <= settings.mist_duration_millis) {
      Serial.printf(
        "Mist interval should be greater than mist duration (got %d, current mist duration is %d)\n", 
        mistMillis, 
        settings.mist_duration_millis);
    } else {
      settings.mist_interval_millis = mistMillis;
      Serial.printf("Setting mist interval to %d ms\n", mistMillis);
    }
    saveConfig();
  });

  client.subscribe(mqttCommandSetMistDurationMillis, [](const String & payload) {
    Serial.printf("got request to change misting duration (%s)\n", payload);
    int mistMillis = payload.toInt();
    if (mistMillis < 500) {
      Serial.printf("Mist duration should be >= 500 ms (got %d)\n", mistMillis);
    } else if (mistMillis > settings.mist_interval_millis) {
      Serial.printf(
        "Mist duration should be less than mist interval (got %d, current mist interval is %d)\n", 
        mistMillis, 
        settings.mist_interval_millis);
    } else {
      settings.mist_duration_millis = mistMillis;
      Serial.printf("Setting mist duration to %d ms\n", mistMillis);
    }
    saveConfig();
  });

  client.subscribe(mqttCommandSetMinPSI, [](const String & payload) {
    Serial.printf("got request to change min PSI (%s)\n", payload);
    int PSI = payload.toInt();
    if (PSI < 10) {
      Serial.printf("Min PSI should be >= 10 (got %d)\n", PSI);
    } else if (PSI >= settings.pump_max_pressure) {
      Serial.printf(
        "Min PSI should be less than max PSI (current max PSI is %d)\n", 
        settings.pump_max_pressure);
    }else {
      settings.pump_min_pressure = PSI;
      Serial.printf("Setting min PSI to %d\n", PSI);
    }
    saveConfig();
  });

  client.subscribe(mqttCommandSetMaxPSI, [](const String & payload) {
    Serial.printf("got request to change max PSI (%s)\n", payload);
    int PSI = payload.toInt();
    if (PSI > 115) {
      Serial.printf("Max PSI should be <= 115 (got %d)\n", PSI);
    } else if (PSI <= settings.pump_min_pressure) {
      Serial.printf(
        "Max PSI should be greater than min PSI (current min PSI is %d)\n", 
        settings.pump_min_pressure);
    }else {
      settings.pump_max_pressure = PSI;
      Serial.printf("Setting max PSI to %d\n", PSI);
    }
    saveConfig();
  });

  // Publish a message to indicate connection
  client.publish("aero/status", "CONNECTED", true);
  client.publish(mqttPumpStatus, pumpOn ? "on" : "off", true);
  publishConfig();
}

void loop() {
  ArduinoOTA.handle();
  client.loop();


// Toggle full drain mode via switch
  if (digitalRead(switchPin) == HIGH)
  {
    if (mistingState != full_drain) {
      mistingState = full_drain;
        Serial.println("starting full drain");
    }
  }
  else {
    if (mistingState == full_drain) {
      mistingState = none;
      Serial.println("stopping full drain");
    }
  }

  if (settings.misting_enabled) {
    updateSolenoids();
  } else {
    digitalWrite(mistSolenoid, LOW);
    digitalWrite(drainSolenoid, LOW);
  }


  float pressure;
  if (measurePressure(&pressure) == true) {
    Serial.printf("pressure reading: %f\n", pressure);
    client.publish(mqttPressure, String(pressure).c_str(), true);
    if (pressure < 80 && !pumpOn) {
      if (lastPressureReadingLow) {
        lastPressureReadingLow = false;
        digitalWrite(pumpRelay, HIGH);
        Serial.println("turning pump on");
        pumpOn = true;
        client.publish(mqttPumpStatus, "on", true);
      } else {
        lastPressureReadingLow = true;
      }
    }
    if (pressure > 100 && pumpOn) {
      if (lastPressureReadingHigh) { // Get two in a row
        lastPressureReadingHigh = false;
        digitalWrite(pumpRelay, LOW);
        pumpOn = false;
        Serial.println("turning pump off");
        client.publish(mqttPumpStatus, "off", true);
      } else {
        lastPressureReadingHigh = true;
      }
    }
  }

  float temperature;
  float humidity;

  /* Measure temperature and humidity.  If the functions returns
     true, then a measurement is available. */
  if( measure_environment( &temperature, &humidity ) == true )
  {
    Serial.print( "T = " );
    Serial.print( temperature, 1 );
    Serial.print( " deg. C (");
    Serial.print( temperature * 9.0/5 + 32, 1);
    Serial.print( " deg. F), H = " );
    Serial.print( humidity, 1 );
    Serial.println( "%" );
    client.publish(mqttTemp1, String(temperature).c_str(), true);
    client.publish(mqttHumidity1, String(humidity).c_str(), true);
  }
}
