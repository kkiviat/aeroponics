# syntax=docker/dockerfile:1

FROM python:3.8-slim-buster

RUN apt-get update -y && apt-get install build-essential -y

WORKDIR /monitor-environment

COPY requirements.txt requirements.txt
RUN pip3 install -r requirements.txt

COPY . .

CMD [ "python3", "./monitorEnvironment.py" ]
