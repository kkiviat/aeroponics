#include <ArduinoOTA.h>
#include <dht_nonblocking.h>
#include "EspMQTTClient.h"
#include <ESP_EEPROM.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>

//#define DEBUG

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

// misting
#define mqttLastMistTime "aero/lastMistTime"
#define mqttNextMistTime "aero/nextMistTime"
#define mqttPiLastMistTime "aeroPi/lastMistTime" // sent from Pi on connection established


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
// Misting state
// =============================
enum MistingState { none, mist, bleed, full_drain, waiting };
MistingState mistingState = waiting; // start in waiting in case pressure is low
int bleedDuration = 500; // 0.5 seconds
unsigned long lastMistTime = 0; // time of last misting in millis since start
int mistStartSeconds = 0; // time of last misting in epoch seconds; 0 means nothing has happened

static void updateSolenoids() {
  static unsigned long bleedStart = 0;

  if (!settings.misting_enabled) {
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
    Serial.printf("Stopping misting after %d millis\n", millis() - lastMistTime);
    Serial.println("Starting bleeding");
    digitalWrite(mistSolenoid, LOW);
    digitalWrite(drainSolenoid, HIGH);
    bleedStart = millis();
    mistingState = bleed;
    return;
  }
  if (mistingState == bleed && millis() - bleedStart > bleedDuration) {
    // Stop draining
    Serial.printf("Stopping bleeding after %d millis\n", millis() - bleedStart);
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
// InfluxDB v2 server url, e.g. https://eu-central-1-1.aws.cloud2.influxdata.com (Use: InfluxDB UI -> Load Data -> Client Libraries)
#define INFLUXDB_URL "https://us-central1-1.gcp.cloud2.influxdata.com"
// InfluxDB v2 server or cloud API authentication token (Use: InfluxDB UI -> Data -> Tokens -> <select token>)
#define INFLUXDB_TOKEN "***REMOVED***"
// InfluxDB v2 organization id (Use: InfluxDB UI -> User -> About -> Common Ids )
#define INFLUXDB_ORG "***REMOVED***"
// InfluxDB v2 bucket name (Use: InfluxDB UI ->  Data -> Buckets)
#define INFLUXDB_BUCKET "aero"

// Set timezone string according to https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
// Examples:
//  Pacific Time: "PST8PDT"
//  Eastern: "EST5EDT"
//  Japanesse: "JST-9"
//  Central Europe: "CET-1CEST,M3.5.0,M10.5.0/3"
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
    return;
  }
      // Print what are we exactly writing
#ifdef DEBUG
    Serial.print("Writing: ");
    Serial.println(point.toLineProtocol());
#endif
  
    // Write point
    if (!influxClient.writePoint(point)) {
      Serial.print("InfluxDB write failed: ");
      Serial.println(influxClient.getLastErrorMessage());
    }
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
static bool measure_environment(float *temperature, float *humidity) {
  static unsigned long measurement_timestamp = millis();

  /* Measure once every four seconds. */
  if (millis( ) - measurement_timestamp > temperatureInterval)
  {
    if (dht_sensor.measure( temperature, humidity))
    {
      measurement_timestamp = millis( );
      return(true);
    }
  }

  return(false);
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
  Serial.printf("Pressure analog reading: %f\n", analogReading);
  float psi = -38.2 + analogReading * 0.285 - 0.0000968 * analogReading * analogReading;

  return(psi);
}

static bool measurePressure(float *pressure) {
  static unsigned long measurement_timestamp = millis();

  float averageReading = 0;
  if (millis() - measurement_timestamp > 5000) {
    for (int i = 0; i < 5; i++) {
      float tempReading = analogRead(pressureSensor);
      averageReading += tempReading;
      //Serial.printf("Reading %d is %f\n", i, tempReading);
    }
    measurement_timestamp = millis();
    *pressure = analogToPSI(averageReading / 5);
    return(true);
  }
  return(false);
}

void logPumpStatus() {
  client.publish(mqttPumpStatus, pumpOn ? "on" : "off", true);

  // Clear fields for reusing the point. Tags will remain untouched
    pumpStatusPoint.clearFields();
  
    // Store measured value into point
    pumpStatusPoint.addField("status", pumpOn ? 1 : 0);
  
    writeToInfluxDB(pumpStatusPoint);
}

void logPressure(float pressure) {
  client.publish(mqttPressure, String(pressure).c_str(), true);

  // Clear fields for reusing the point. Tags will remain untouched
  pressurePoint.clearFields();

  // Store measured value into point
  pressurePoint.addField("psi", pressure);

  writeToInfluxDB(pressurePoint);
}

// =============================
// OTA
// =============================
// This function is called once everything is connected (Wifi and MQTT)
// WARNING : YOU MUST IMPLEMENT IT IF YOU USE EspMQTTClient
void onConnectionEstablished() {
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
  
  // Subscribe to "mytopic/test" and display received message to Serial
//  client.subscribe("mytopic/test", [](const String & payload) {
//    Serial.println(payload);
//  });

  // Subscribe to "mytopic/wildcardtest/#" and display received message to Serial
//  client.subscribe("mytopic/wildcardtest/#", [](const String & topic, const String & payload) {
//    Serial.println("(From wildcard) topic: " + topic + ", payload: " + payload);
//  });

  // Accurate time is necessary for certificate validation and writing in batches
  // For the fastest time sync find NTP servers in your area: https://www.pool.ntp.org/zone/
  // Syncing progress and the time will be printed to Serial.
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

  // Check InfluxDB server connection
  if (influxClient.validateConnection()) {
    Serial.print("Connected to InfluxDB: ");
    Serial.println(influxClient.getServerUrl());
  } else {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(influxClient.getLastErrorMessage());
  }

  client.subscribe(mqttPiLastMistTime, [](const String & payload) {
    Serial.printf("got last mist time (%s)\n", payload);
    int receivedMistTime = payload.toInt();
    if (receivedMistTime > mistStartSeconds) {
      mistStartSeconds = receivedMistTime;
      // Set the lastMistTime millis based on the time since the last recorded misting
      int millisSinceMisting = (time(nullptr) - mistStartSeconds) * 1000;
      if (millisSinceMisting > 0) {
        lastMistTime = millis() - millisSinceMisting;
        Serial.printf("Setting last mist time millis to %d\n", lastMistTime);
      } else { // something went wrong getting current time
        mistStartSeconds = -1;
        Serial.printf("Not setting last mist time since it appears to be in the future (time now: %s)\n",
          String(time(nullptr)));
      }
    } else {
      mistStartSeconds = -1;
      Serial.println("Received invalid misting time; ignoring");
    }
  });

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
    // update next misting time
    client.publish(mqttNextMistTime, String(mistStartSeconds + settings.mist_interval_millis / 1000.0).c_str());
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

void loop() {
  ArduinoOTA.handle();
  client.loop();

  if (client.getConnectionEstablishedCount() == 0 && millis() < 5000) {
    // wait 5 seconds for connections to be established
    return;
  }

  // Wait a short time after startup to receive last mist time from pi.
  // Otherwise, act like we haver never misted.
  if (client.isConnected() && mistStartSeconds == 0) {
    // means we have never misted and have never received last mist time from pi
    if (millis() < 10000) {
      // wait a few seconds for pi to send last mist time
      return;
    }
    if (mistStartSeconds == 0) {
      mistStartSeconds = -1; // give up on waiting
    }
  }

  float pressure;
  if (measurePressure(&pressure) == true) {
    Serial.printf("pressure reading: %f\n", pressure);
    logPressure(pressure);
    if (pressure < settings.pump_min_pressure && !pumpOn) {
      if (lastPressureReadingLow) {
        lastPressureReadingLow = false;
        digitalWrite(pumpRelay, HIGH);
//#ifdef DEBUG
        Serial.println("turning pump on");
//#endif
        pumpOn = true;
        logPumpStatus();
      } else {
        lastPressureReadingLow = true;
      }
    }
    if (pressure > settings.pump_max_pressure && pumpOn) {
      if (lastPressureReadingHigh) { // Get two in a row
        lastPressureReadingHigh = false;
        digitalWrite(pumpRelay, LOW);
        pumpOn = false;
//#ifdef DEBUG
        Serial.println("turning pump off");
//#endif
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
      Serial.println("changing mister state to waiting due to low pressure");
    } else if (mistingState == waiting && pressure >= settings.pump_min_pressure && mistStartSeconds != 0) {
      // can start once pressure is high enough and we are not still waiting for last mist time from pi
      mistingState = none;
      Serial.println("Pressure high enough; leaving waiting state");
    }
  }

  updateSolenoids();

  float temperature;
  float humidity;

  /* Measure temperature and humidity.  If the functions returns
     true, then a measurement is available. */
  if( measure_environment( &temperature, &humidity ) == true )
  {
#ifdef DEBUG
    Serial.print( "T = " );
    Serial.print( temperature, 1 );
    Serial.print( " deg. C (");
    Serial.print( temperature * 9.0/5 + 32, 1);
    Serial.print( " deg. F), H = " );
    Serial.print( humidity, 1 );
    Serial.println( "%" );
#endif
    client.publish(mqttTemp1, String(temperature).c_str(), true);
    client.publish(mqttHumidity1, String(humidity).c_str(), true);

    // Clear fields for reusing the point. Tags will remain untouched
    ambientPoint.clearFields();
  
    // Store measured value into point
    ambientPoint.addField("temperature", temperature);
    ambientPoint.addField("humidity", humidity);

    writeToInfluxDB(ambientPoint);
  }
}
