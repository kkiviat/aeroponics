#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include "EspMQTTClient.h"
#include <uFire_Mod-EC.h>
#include <uFire_Mod-pH.h>

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
// pH and EC
// =============================
uFire::Mod_EC::i2c ec;
uFire::Mod_pH::i2c ph;

// Calibration
#define PH_HIGH_SOLUTION 7.0
#define PH_LOW_SOLUTION  4.0
#define EC_HIGH_SOLUTION 10.0
#define EC_LOW_SOLUTION  1.0

#define PH_MEASURE_INTERVAL 5000
#define EC_MEASURE_INTERVAL 5000

static bool measurepH(float *pH) {
  static unsigned long measurementTimestamp = millis();

  if (millis() - measurementTimestamp > PH_MEASURE_INTERVAL) {
    measurementTimestamp = millis();
    *pH = ph.measurepH(ec.measureTemp());
    return(true);
  }
  return(false);
}

static bool measureEC(float *EC) {
  static unsigned long measurementTimestamp = millis();

  if (millis() - measurementTimestamp > EC_MEASURE_INTERVAL) {
    measurementTimestamp = millis();
    *EC = ec.measureEC(ec.measureTemp());
    return(true);
  }
  return(false);
}

// =============================
// MQTT
// =============================

// MQTT topics
#define MQTT_PH "aero/pH"
#define MQTT_EC "aero/EC"
#define MQTT_CALIBRATE "aeroRes/calibrate"
#define MQTT_RES_TEMP "aero/reservoirTemp"


#define MQTT_SERVER "192.168.0.134"
#define MQTT_CLIENT "ReservoirClient"
#define WILL_TOPIC "aeroRes/status"
#define WILL_MESSAGE "DISCONNECTED"

EspMQTTClient mqttClient(
  SSID,
  WIFI_PWD,
  MQTT_SERVER,
  MQTT_USERNAME,
  MQTT_PASSWD,
  MQTT_CLIENT
);

void calibrateECHigh() {
  Serial.println("Calibrating EC High");
  ec.calibrateHigh(EC_HIGH_SOLUTION, ec.measureTemp());
  Serial.println("done");
}

void calibrateECLow() {
  Serial.println("Calibrating EC Low");
  ec.calibrateHigh(EC_LOW_SOLUTION, ec.measureTemp());
  Serial.println("done");
}

void calibratePHHigh() {
  Serial.println("Calibrating pH High");
  ph.calibrateHigh(PH_HIGH_SOLUTION, ph.measureTemp());
  Serial.println("done");
}

void calibratePHLow() {
  Serial.println("Calibrating pH Low");
  ph.calibrateHigh(PH_LOW_SOLUTION, ph.measureTemp());
  Serial.println("done");
}

void onConnectionEstablished() {
  mqttClient.subscribe(MQTT_CALIBRATE, [](const String & payload) {
    debug_println(payload);
    if (payload.equals("EC_HIGH")) {
      //calibrateECHigh();
    }
    if (payload.equals("EC_LOW")) {
      //calibrateECLow();
    }
    if (payload.equals("PH_HIGH")) {
      //calibratePHHigh();
    }
    if (payload.equals("PH_LOW")) {
      //calibratePHLow();
    }
  });
  
  mqttClient.publish(WILL_TOPIC, "CONNECTED", true);
}

void logpH(float measuredpH) {
  mqttClient.publish(MQTT_PH, String(measuredpH, 2));
  debug_printf("measured pH: %.2f\n", measuredpH);
}

void logEC(float measuredEC) {
  mqttClient.publish(MQTT_EC, String(measuredEC, 2));
  debug_printf("measured EC: %.2f\n", measuredEC);
}

void logTemp(float tempC) {
  mqttClient.publish(MQTT_RES_TEMP, String(tempC, 2));
  debug_printf("measured temp: %.2f\n", tempC);
}

// =============================
// mDNS
// =============================
void startmDNS() {
  if (!MDNS.begin("reservoir-module")) {
    Serial.println("Error setting up mDNS!");
  }
  debug_println("mDNS started");
}

// =============================
// OTA
// =============================
void setupOTA() {
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
}

void setup() {
  Serial.begin(115200);

  mqttClient.enableLastWillMessage(WILL_TOPIC, WILL_MESSAGE);
  setupOTA();
  startmDNS();

  Wire.begin();
  if (!ec.begin()) {
    Serial.println("EC module not detected");
  }
  if (!ph.begin()) {
    Serial.println("pH module not detected");
  }
}


void loop() {
  ArduinoOTA.handle();
  mqttClient.loop();

  float measuredpH;
  float measuredEC;

  if (measurepH(&measuredpH)) {
    logpH(measuredpH);
  }
  if (measureEC(&measuredEC)) {
    logEC(measuredEC);
    logTemp(ec.tempC);
  }

}
