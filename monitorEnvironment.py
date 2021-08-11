#!/usr/local/bin/python

import RPi.GPIO as GPIO
import time
import board
import adafruit_dht
from datetime import datetime

import paho.mqtt.publish as publish

from influxdb_client import InfluxDBClient, Point, WritePrecision
from influxdb_client.client.write_api import SYNCHRONOUS
import influxConfig as cfg

client = InfluxDBClient(url="https://us-central1-1.gcp.cloud2.influxdata.com", token=cfg.token)
write_api = client.write_api(write_options=SYNCHRONOUS)

GPIO.setmode(GPIO.BCM)

LIGHT_PIN = 4
DHT_PIN = board.D27

LIGHT_READ_INTERVAL = 20 # seconds
DHT_READ_INTERVAL = 30 # seconds

last_light_read_time = 0
last_dht_read_time = 0

dhtDevice = adafruit_dht.DHT11(DHT_PIN, use_pulseio=False)

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
            point = Point("light") \
                .tag("host", "raspberrypi") \
                .field("light", light) \
                .time(datetime.utcnow(), WritePrecision.NS) 
            write_api.write(cfg.bucket, cfg.org, point)
            publish.single("aero/light", light, hostname="localhost")
            last_light_read_time = time.time()

        if time.time() - last_dht_read_time > DHT_READ_INTERVAL:
            result = readDHT11()
            if result is not None:
                last_dht_read_time = time.time()
                temperature = result[0]
                humidity = result[1]
                point = Point("ambient") \
                    .tag("host", "raspberrypi") \
                    .field("temperature", temperature) \
                    .field("humidity", humidity) \
                    .time(datetime.utcnow(), WritePrecision.NS) 
                write_api.write(cfg.bucket, cfg.org, point)
                publish.single("aero/ambientTemp", temperature, hostname="localhost")
                publish.single("aero/ambientHumidity", humidity, hostname="localhost")
 
except KeyboardInterrupt:
    pass
finally:
    GPIO.cleanup()
