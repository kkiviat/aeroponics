#!/usr/local/bin/python
import paho.mqtt.publish as publish
import paho.mqtt.client as mqtt
import RPi.GPIO as GPIO
import os
import time
import board
import adafruit_dht
from datetime import datetime
from influxdb import InfluxDBClient

username = os.environ['MQTT_USER']
password = os.environ['MQTT_PASS']

influx_client = InfluxDBClient('influxdb', 8086, database='aero')

mqttClient = mqtt.Client()
mqttClient.username_pw_set(username, password=password)
mqttClient.connect("mosquitto", 1883, 60)

GPIO.setmode(GPIO.BCM)

LIGHT_PIN = 4
DHT_PIN = board.D27

LIGHT_READ_INTERVAL = 20 # seconds
DHT_READ_INTERVAL = 30 # seconds

last_light_read_time = 0
last_dht_read_time = 0

dhtDevice = adafruit_dht.DHT11(DHT_PIN, use_pulseio=False)

def logTempAndHumidity(temp, humidity):
    current_time = datetime.utcnow().isoformat()
    json_body = [
        {
            "measurement": "ambient",
            "tags": {},
            "time": current_time,
            "fields": {
                "temperature": temperature,
                "humidity": humidity,
            }
        }
    ]
    influx_client.write_points(json_body)
    mqttClient.publish("aero/ambientTemp", temperature)
    mqttClient.publish("aero/ambientHumidity", humidity)


def logLight(light):
    current_time = datetime.utcnow().isoformat()
    json_body = [
        {
            "measurement": "light",
            "tags": {},
            "time": current_time,
            "fields": {
                "value": light
            }
        }
    ]
    influx_client.write_points(json_body)
    mqttClient.publish("aero/light", light)

def readDHT11():
    try:
        temperature_c = dhtDevice.temperature
        temperature_f = temperature_c * (9 / 5) + 32
        humidity = dhtDevice.humidity
        return temperature_f, humidity
 
    except RuntimeError as error:
        print(error.args[0])
        return None
    except Exception as error:
        dhtDevice.exit()
        raise error


# Uses timing of an RC circuit
# Absolute values don't have a particular meaning
def readLight(pin):
    # Output briefly to discharge capacitor
    GPIO.setup(pin, GPIO.OUT)
    GPIO.output(pin, GPIO.LOW)
    time.sleep(0.1)

    GPIO.setup(pin, GPIO.IN)
    # Measure time elapsed until pin reads high
    start = time.time()
    while (GPIO.input(pin) == GPIO.LOW):
        pass
    end = time.time()

    return 1000 * (end - start)

try:
    while True:
        if time.time() - last_light_read_time > LIGHT_READ_INTERVAL:
            light = readLight(LIGHT_PIN)
            logLight(light)
            last_light_read_time = time.time()

        if time.time() - last_dht_read_time > DHT_READ_INTERVAL:
            result = readDHT11()
            if result is not None:
                last_dht_read_time = time.time()
                temperature = result[0]
                humidity = result[1]
                logTempAndHumidity(temperature, humidity)
 
except KeyboardInterrupt:
    pass
finally:
    GPIO.cleanup()
