![aeroponics](https://github.com/kkiviat/aeroponics/assets/20493906/5a11c7b3-8d03-4323-8fda-908147cdb5a4)
# Hardware
## Sensor system
- [Microfire](https://microfire.co/) EC and pH sensors
- [ESP8266](https://www.amazon.com/gp/product/B081CSJV2V)
- [Pressure sensor](https://www.amazon.com/gp/product/B0748CV4G1)
- [Mini 3A step-down power module](https://www.amazon.com/gp/product/B07JWGN1F6)
- [Waterproof thermometer](https://www.amazon.com/gp/product/B012C597T0)
![sensors](https://github.com/kkiviat/aeroponics/assets/20493906/11f869db-4114-4e2b-8c49-2045d5d8e2bc)

## Misting system
- [ESP8266](https://www.amazon.com/gp/product/B081CSJV2V)
- [5 V Relay modules](https://www.amazon.com/gp/product/B079FGPC9Y?th=1)
- [12 V Solenoid valves](https://www.amazon.com/gp/product/B07H2R41J9)
- [2 G accumulator tank](https://www.amazon.com/gp/product/B00IRFW38W)
- [Aquatec 6800 booster pump](https://www.amazon.com/gp/product/B074PCVH36)
![misting](https://github.com/kkiviat/aeroponics/assets/20493906/cc86ab4f-e453-4b29-bb64-b9dd036c9185)

## Other
- Raspberry Pi 3B+  (for monitoring)
- [Raspberry Pi IR camera](https://www.amazon.com/gp/product/B07BK1QZ2L) (for time-lapse snapshots
- [Briignite 800 W LED grow light](https://www.amazon.com/gp/product/B08VGFHSW8)


# Software
Uses MQTT to monitor and control misting cycle through local network. Generates graphs and alerts via InfluxDB and Grafana running on a Raspberry Pi 3.
