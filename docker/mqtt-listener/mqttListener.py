import paho.mqtt.client as mqtt
from influxdb import InfluxDBClient
import datetime
import logging
import os

username = os.environ['MQTT_USER']
password = os.environ['MQTT_PASS']

lastMistTime = 0

def writeToInfluxDB(msg):
    measurement = msg.topic.split('/')[-1]
    if measurement == "pump":
        value = 1 if msg.payload == b"on" else 0
    elif measurement == "pressure":
        value = float(msg.payload)
    elif measurement == "mistersOn":
        value = int(msg.payload) # either 1 or 0
    else: # not a message we log
        return
    current_time = datetime.datetime.utcnow().isoformat()
    json_body = [
        {
            "measurement": measurement,
            "tags": {},
            "time": current_time,
            "fields": {
                "value": value
            }
        }
    ]
    logging.info(json_body)
    influx_client.write_points(json_body)


logging.basicConfig(level=logging.INFO)
influx_client = InfluxDBClient('influxdb', 8086, database='aero')

def handleStatus(status):
    if status == b"CONNECTED":
        logging.info("sending lastMistTime="+str(lastMistTime))
        client.publish("aeroPi/lastMistTime", lastMistTime)

def on_connect(client, userdata, flags, rc):
    client.subscribe("aero/#")

def on_message(client, userdata, msg):
    logging.info(msg.topic+" "+str(msg.payload))
    if msg.topic == "aero/status":
        logging.info("got aero/status")
        handleStatus(msg.payload)
    elif msg.topic == "aero/lastMistTime":
        logging.info("saving lastMistTime")
        global lastMistTime
        lastMistTime = int(msg.payload)
    else:
        writeToInfluxDB(msg)

client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message
client.username_pw_set(username, password=password)
client.connect("mosquitto", 1883, 60)

client.loop_forever()
