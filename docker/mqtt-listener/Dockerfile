# syntax=docker/dockerfile:1

FROM python:3.8-slim-buster

WORKDIR /mqtt-listener

COPY requirements.txt requirements.txt
RUN pip3 install -r requirements.txt

COPY . .

CMD [ "python3", "./mqttListener.py" ]
