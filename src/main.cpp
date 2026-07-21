#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Ping.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <AsyncMqttClient.h>
#include <time.h>
#include <RTClib.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include "debounce.h"
#include "ConnectionManager.h"
#include "EventLogger.h"
#include "configuration.h"
// #include "dev_configuration.h"

WebServer localWebServer(80);
AsyncMqttClient mqttClient;
InfluxDBClient influxLogClient(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_LOG_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
EventLogger eventLog(influxLogClient, -1);
ConnectionManager internetConnectionManager(eventLog, primaryDNS, secondaryDNS);

Debounce MQTTReconnect(10000);
u_int8_t mqttReconnectAttempts = 0;
u_int8_t maxMqttReconnectAttempts = 50;
Debounce onboardButtonDebounce(500);
Debounce privacyButtonDebounce(500);
Debounce inverterPowerChangeInterval(INV_RETRY_PERIOD * 1000);
Debounce NTPSyncInterval(NTP_SYNC_INTERVAL * 3600000);

enum powerSwitch
{
  OFF,
  ON
};

bool onboardLEDState;
volatile bool onboardButtonPushed;
volatile bool privacyButtonPushed;
volatile powerSwitch cameraTarget = ON;
powerSwitch cameraState = ON;
powerSwitch inverterPowerState = ON;
powerSwitch xmasLightState = OFF;

void reconnectMqtt();
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total);
void controlInverter();
void controlLight();
void controlCamera();
void IRAM_ATTR privacyButtonPush();
void privacyButtonAction();
void IRAM_ATTR onboardButtonPush();
void onboardButtonAction();

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("====================");
  Serial.println(" System is starting ");
  Serial.println("====================");

  pinMode(ONBOARD_BUTTON, INPUT_PULLUP);
  pinMode(ONBOARD_LED, OUTPUT);
  pinMode(RELAY_INV, OUTPUT);
  pinMode(RELAY_LIGHT, OUTPUT);
  pinMode(RELAY_CAM, OUTPUT);
  pinMode(RELAY_AUX, OUTPUT);
  pinMode(LED_PRIVACY_BUTTON, OUTPUT);
  pinMode(PRIVACY_BUTTON, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(ONBOARD_BUTTON), onboardButtonPush, FALLING);
  attachInterrupt(digitalPinToInterrupt(PRIVACY_BUTTON), privacyButtonPush, FALLING);

  // Make sure relay positions match the corresponding power switch
  digitalWrite(RELAY_INV, HIGH);                                  // NC
  digitalWrite(RELAY_LIGHT, (xmasLightState == ON) ? HIGH : LOW); // NO
  digitalWrite(RELAY_CAM, (cameraState == ON) ? LOW : HIGH);      // NC
  digitalWrite(RELAY_AUX, LOW);
  digitalWrite(LED_PRIVACY_BUTTON, (cameraState == ON) ? LOW : HIGH);

  internetConnectionManager.begin(ssid, password, hostname);

  timeSync(TIME_ZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);

  // OTA
  localWebServer.on("/", []()
                    { String websiteContents = "Tere tulemast Eesti saatkonda!\nJaotis: " + String(hostname);
                      localWebServer.send(200, "text/plain", websiteContents); });
  ElegantOTA.setAuth(otaUsername, otaPassword);
  ElegantOTA.begin(&localWebServer);
  localWebServer.begin();
  eventLog.log("Webbserver startad", EventLogger::LogLevel::INFO);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.setCredentials(MQTT_USER, MQTT_PASS);
  mqttClient.setServer(MQTT_HOST, 1883);
  mqttClient.setWill(esp32_status_topic, 1, true, "offline");
  mqttClient.connect();

  delay(1000);

  eventLog.sendPendingPoints();

  float setupTime = millis() / 1000.0f;
  eventLog.log(String("Systemet startat. Uppstarten tog " + String(setupTime) + " s."), EventLogger::LogLevel::INFO);
}

void loop()
{
  internetConnectionManager.loop();

  if (!mqttClient.connected())
    reconnectMqtt();

  localWebServer.handleClient();
  ElegantOTA.loop();

  if (onboardButtonPushed)
    onboardButtonAction();

  if (privacyButtonPushed)
    privacyButtonAction();

  controlCamera();

  if (NTPSyncInterval.ready())
    timeSync(TIME_ZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);

  yield();
}

void reconnectMqtt()
{
  if (!MQTTReconnect.ready())
    return;

  if (!internetConnectionManager.isConnected()) // Need WiFi to connect to MQTT broker
    return;

  if (mqttReconnectAttempts++ > maxMqttReconnectAttempts)
  {
    eventLog.log("MQTT: För många misslyckade försök att ansluta till broker. Startar om", EventLogger::LogLevel::INFO);
    ESP.restart();
  }

  eventLog.log("MQTT: Försöker återansluta till broker...", EventLogger::LogLevel::INFO);
  mqttClient.connect();
}

void onMqttConnect(bool sessionPresent)
{
  uint16_t packetId = mqttClient.subscribe(camera_command_topic, 1);
  mqttClient.publish(esp32_status_topic, 1, true, "online");
  mqttClient.publish(camera_state_topic, 1, true, cameraState == ON ? "ON" : "OFF");

  eventLog.log("MQTT: Ansluten till broker", EventLogger::LogLevel::INFO);
  eventLog.log(String("MQTT: Prenumererar på " + String(camera_command_topic)), EventLogger::LogLevel::INFO);
  eventLog.log(String("MQTT: Subscribe packet ID " + String(packetId)), EventLogger::LogLevel::INFO);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  String message = "Frånkopplad från MQTT-broker p.g.a.: ";
  message += static_cast<int>(reason);
  eventLog.log(message, EventLogger::LogLevel::WARNING);

  if (reason != AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED)
    reconnectMqtt();
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total)
{
  if (strcmp(topic, camera_command_topic) != 0)
    return;

  String message;
  for (size_t i = 0; i < len; i++)
    message += (char)payload[i];

  if (message == "ON")
  {
    cameraTarget = ON;
    eventLog.log("MQTT: Kamera ON-kommando mottaget", EventLogger::LogLevel::INFO, true);
  }
  else if (message == "OFF")
  {
    cameraTarget = OFF;
    eventLog.log("MQTT: Kamera OFF-kommando mottaget", EventLogger::LogLevel::INFO, true);
  }
}

void controlInverter()
{
  if (!inverterPowerChangeInterval.ready())
    return;
  /*
  const auto &intData = mpptData.getIntMap(); // Battery voltage in mV. Using MPPT voltage, since Inv. voltage = 0 when off

  auto it = intData.find("V");
  if (it == intData.end())
  {
    eventLog.log("Hittade ingen batterispänning från MPPT", EventLogger::LogLevel::WARNING);
    return;
  }
  const int voltage = it->second;

  // Do nothing on coco-bananas values (<1 V or >20 V)
  if (voltage < 1000 || voltage > 20000)
  {
    String messageText = "Orealistikt spänningsvärde (" + String(voltage) + " mV), ändrar inte status på inverter";
    eventLog.log(messageText, EventLogger::LogLevel::WARNING);
    return;
  }

  // If battery voltage is lower than off voltage, turn inverter off
  if (inverterPowerState == ON && voltage < INV_OFF_VOLTAGE)
  {
    digitalWrite(RELAY_INV, HIGH); // Relay is NC, so triggering it will turn off the inverter
    inverterPowerState = OFF;
    eventLog.log("Inverter stängdes av, låg batterispänning", EventLogger::LogLevel::INFO);
    return;
  }

  // If battery voltage is higher than on voltage, turn inverter on
  if (inverterPowerState == OFF && voltage > INV_ON_VOLTAGE)
  {
    digitalWrite(RELAY_INV, LOW); // Relay is NC, so releasing it will turn on the inverter
    inverterPowerState = ON;
    eventLog.log("Inverter slogs på, tillräcklig batterispänning ", EventLogger::LogLevel::INFO);
    return;
  }
  */
}

/**
 * @brief Turns on light (by triggering a relay) when the following conditions are met:
 *        - It is christmas time (between December 1 and January 13)
 *        - It is daytime (between 08:00 and 20:00)
 *        - It is dark (the panel voltage of the photovoltaic panel is < 5000 mV)
 */
void controlLight()
{
  /*
  const auto &intData = mpptData.getIntMap(); // Contains panel voltage (VPV) in mV

  auto it = intData.find("VPV");
  if (it == intData.end())
  {
    eventLog.log("Hittade ingen panelspänning från MPPT", EventLogger::LogLevel::WARNING);
    return;
  }

  const int panelVoltage = it->second;

  time(&now);
  struct tm *timeinfo = localtime(&now);

  const bool isXmas = (timeinfo->tm_mon == 11 && timeinfo->tm_mday >= 1) || (timeinfo->tm_mon == 0 && timeinfo->tm_mday <= 13);
  const bool isDark = (panelVoltage < 5000);
  const bool isDay = (timeinfo->tm_hour >= 8) && (timeinfo->tm_hour < 20);

  xmasLightState = (isXmas && isDark && isDay) ? ON : OFF;

  xmasLightState ? digitalWrite(RELAY_LIGHT, HIGH) : digitalWrite(RELAY_LIGHT, LOW);
  */
}

void controlCamera()
{
  if (cameraState == cameraTarget)
    return;

  cameraState = cameraTarget;

  Serial.println("Camera state change");
  Serial.print("Camera target is: ");
  Serial.println(cameraTarget);
  switch (cameraState)
  {
  case ON:
    digitalWrite(LED_PRIVACY_BUTTON, LOW); // Green light off when cam on
    digitalWrite(RELAY_CAM, LOW);          // Relay is NC
    mqttClient.publish(camera_state_topic, 1, true, "ON");
    eventLog.log("Camera turned on", EventLogger::LogLevel::INFO, true);
    break;

  case OFF:
    digitalWrite(LED_PRIVACY_BUTTON, HIGH); // Green light indicates cam is OFF
    digitalWrite(RELAY_CAM, HIGH);          // Relay is NC
    mqttClient.publish(camera_state_topic, 1, true, "OFF");
    eventLog.log("Camera turned off", EventLogger::LogLevel::INFO, true);
    break;

  default:
    Serial.print("Camera target was ");
    Serial.println(cameraTarget);
    break;
  }
}

void IRAM_ATTR onboardButtonPush()
{
  if (onboardButtonDebounce.ready())
    onboardButtonPushed = true;
}

void onboardButtonAction()
{
  onboardButtonPushed = false;
  Serial.println("========== DIAGNOSTIK ==========");
  Serial.println("Uptime:       " + String(millis() / 1000) + " s");
  Serial.println("WiFi:         " + String(WiFi.isConnected() ? "Ansluten" : "Frånkopplad"));
  Serial.println("WiFi-signal:  " + String(WiFi.RSSI()) + " dBm");
  Serial.println("MQTT:         " + String(mqttClient.connected() ? "Ansluten" : "Frånkopplad"));
  Serial.println("Fri heap:     " + String(ESP.getFreeHeap()) + " bytes");
  Serial.println("Kamera:       " + String(cameraState == ON ? "ON" : "OFF"));
  Serial.println("----- Ping -----");
  Serial.println("Gateway:      " + String(Ping.ping(WiFi.gatewayIP()) ? "OK" : "FAIL"));
  Serial.println("Primär DNS:   " + String(Ping.ping(primaryDNS) ? "OK" : "FAIL"));
  Serial.println("Sekundär DNS: " + String(Ping.ping(secondaryDNS) ? "OK" : "FAIL"));
  Serial.println("MQTT-broker:  " + String(Ping.ping(MQTT_HOST) ? "OK" : "FAIL"));
  Serial.println("================================");
}

void IRAM_ATTR privacyButtonPush()
{
  if (privacyButtonDebounce.ready())
    privacyButtonPushed = true;
}

void privacyButtonAction()
{
  privacyButtonPushed = false;
  cameraTarget = (cameraTarget == ON) ? OFF : ON;
  eventLog.log("Privacy button pushed", EventLogger::LogLevel::INFO, true);
}
