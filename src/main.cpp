#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
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
#include "EventLogger.h"
#include "espNowTypdef.h"
#include "configuration.h"
// #include "dev_configuration.h"

WebServer localWebServer(80);
AsyncMqttClient mqttClient;
InfluxDBClient influxLogClient(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_LOG_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
EventLogger eventLog(influxLogClient, -1, "/system.log", hostname);

volatile bool firstConnection = true;
volatile bool hasConnectionProblem = true;
volatile bool connectionProblemIsNew = false;
Debounce connectionProblemsTimeout(30 * 60 * 1000); // half an hour

volatile InverterAction receivedInverterCommand = InverterAction::NO_CHANGE;
volatile bool newInverterCommandAvailable = false;

Debounce mqttReconnect(10000);
Debounce onboardButtonDebounce(500);
Debounce privacyButtonDebounce(500);
Debounce inverterPowerChangeInterval(INV_RETRY_PERIOD * 1000);
Debounce NTPSyncInterval(NTP_SYNC_INTERVAL * 3600000);

enum powerSwitch
{
  OFF,
  ON,
  AUTO
};

bool onboardLEDState;
volatile bool onboardButtonPushed;
volatile bool privacyButtonPushed;
volatile powerSwitch cameraTarget = ON;
powerSwitch cameraState = ON;
volatile powerSwitch inverterPowerMode = ON;
powerSwitch inverterPowerState = ON;
powerSwitch inverterPowerTarget = ON;
powerSwitch xmasLightState = OFF;

void reconnectMqtt();
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total);
void onWifiEvent(WiFiEvent_t event);
void controlInverter();
void controlLight();
void controlCamera();
void IRAM_ATTR privacyButtonPush();
void privacyButtonAction();
void IRAM_ATTR onboardButtonPush();
void onboardButtonAction();
void onEspNowDataReceived(const uint8_t *mac_addr, const uint8_t *incomingData, int len);

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
  digitalWrite(RELAY_INV, LOW);                                   // NC
  digitalWrite(RELAY_LIGHT, (xmasLightState == ON) ? HIGH : LOW); // NO
  digitalWrite(RELAY_CAM, (cameraState == ON) ? LOW : HIGH);      // NC
  digitalWrite(RELAY_AUX, LOW);
  digitalWrite(LED_PRIVACY_BUTTON, (cameraState == ON) ? LOW : HIGH);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.onEvent(onWifiEvent);
  WiFi.begin(ssid, password);
  influxLogClient.setHTTPOptions(HTTPOptions().httpReadTimeout(500));

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

  delay(1000);

  float setupTime = millis() / 1000.0f;
  eventLog.log(String("Systemet startat. Uppstarten tog " + String(setupTime) + " s."), EventLogger::LogLevel::INFO);
}

void loop()
{
  if (hasConnectionProblem)
  {
    if (connectionProblemIsNew)
    {
      connectionProblemsTimeout.reset();
      connectionProblemIsNew = false;
    }

    if (connectionProblemsTimeout.ready())
      ESP.restart();
  }

  if (WiFi.isConnected())
  {
    if (NTPSyncInterval.ready() || firstConnection)
      timeSync(TIME_ZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);

    if (firstConnection)
    {
      eventLog.log("Ansluten till WiFi " + String(ssid), EventLogger::LogLevel::INFO);

      Point netStat("Network");
      netStat.addTag("hostname", WiFi.getHostname());
      netStat.addTag("device", WiFi.getHostname());
      netStat.addField("Channel", WiFi.channel());
      netStat.addField("IP address", WiFi.localIP().toString());
      netStat.addField("Gateway", WiFi.gatewayIP().toString());
      netStat.addField("MAC address", WiFi.macAddress());
      eventLog.writePoint(netStat);

      mqttClient.connect();

      if (esp_now_init() != ESP_OK)
        eventLog.log("ESP-NOW: Fel vid initialization");

      esp_now_register_recv_cb(onEspNowDataReceived);

      firstConnection = false;
    }

    if (!mqttClient.connected())
      reconnectMqtt();

    localWebServer.handleClient();
    ElegantOTA.loop();
    eventLog.maintain();
  }

  if (onboardButtonPushed)
    onboardButtonAction();

  if (privacyButtonPushed)
    privacyButtonAction();

  controlCamera();
  controlInverter();

  yield();
}

void onWifiEvent(WiFiEvent_t event)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    hasConnectionProblem = true;
    break;

  default:
    break;
  }
}

void reconnectMqtt()
{
  if (!mqttReconnect.ready())
    return;

  mqttClient.connect();
}

void onMqttConnect(bool sessionPresent)
{
  hasConnectionProblem = false;
  connectionProblemIsNew = true;

  mqttClient.subscribe(camera_command_topic, 1);
  mqttClient.subscribe(inverter_mode_command_topic, 1);
  mqttClient.publish(esp32_status_topic, 1, true, "online");
  mqttClient.publish(camera_state_topic, 1, true, cameraState == ON ? "ON" : "OFF");
  mqttClient.publish(inverter_power_state_topic, 1, true, inverterPowerState == ON ? "ON" : "OFF");

  eventLog.log("MQTT: Ansluten till broker", EventLogger::LogLevel::INFO);
  eventLog.log(String("MQTT: Prenumererar på " + String(camera_command_topic)), EventLogger::LogLevel::INFO);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  hasConnectionProblem = true;

  String message = "Frånkopplad från MQTT-broker p.g.a.: ";
  message += static_cast<int>(reason);
  eventLog.log(message, EventLogger::LogLevel::WARNING);

  if (reason != AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED)
    reconnectMqtt();
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total)
{
  if (strcmp(topic, camera_command_topic) == 0)
  {
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
  
  if (strcmp(topic, inverter_mode_command_topic) == 0)
  {
    String message;
    for (size_t i = 0; i < len; i++)
    message += (char)payload[i];
    if (message == "ON")
    {
      inverterPowerMode = ON;
      mqttClient.publish(inverter_mode_state_topic, 1, true, "ON");
      eventLog.log("MQTT: Inverter ON-kommando mottaget", EventLogger::LogLevel::INFO, true);
    }
    else if (message == "OFF")
    {
      inverterPowerMode = OFF;
      mqttClient.publish(inverter_mode_state_topic, 1, true, "OFF");
      eventLog.log("MQTT: Inverter OFF-kommando mottaget", EventLogger::LogLevel::INFO, true);
    }
    else if (message == "AUTO")
    {
      inverterPowerMode = AUTO;
      mqttClient.publish(inverter_mode_state_topic, 1, true, "AUTO");
      eventLog.log("MQTT: Inverter AUTO-kommando mottaget", EventLogger::LogLevel::INFO, true);
    }
  }
}

void controlInverter()
{
  if (inverterPowerMode == ON)
    inverterPowerTarget = ON;

  if (inverterPowerMode == OFF)
    inverterPowerTarget = OFF;

  if (inverterPowerMode == AUTO)
  {
    switch (receivedInverterCommand)
    {
    case InverterAction::TURN_ON:
      inverterPowerTarget = ON;
      break;
    case InverterAction::TURN_OFF:
      inverterPowerTarget = OFF;
      break;

    default:
      break;
    }
  }

  if (inverterPowerState == inverterPowerTarget)
    return;

  if (!inverterPowerChangeInterval.ready())
    return;

  switch (inverterPowerTarget)
  {
  case ON:
    digitalWrite(RELAY_INV, LOW); // Relay is NC, so releasing it will turn the inverter ON
    inverterPowerState = ON;
    mqttClient.publish(inverter_power_state_topic, 1, true, "ON");
    eventLog.log("Inverter slogs på ", EventLogger::LogLevel::INFO);
    break;

  case OFF:
    digitalWrite(RELAY_INV, HIGH); // Relay is NC, so triggering it will turn the inverter OFF
    inverterPowerState = OFF;
    mqttClient.publish(inverter_power_state_topic, 1, true, "OFF");
    eventLog.log("Inverter stängdes av", EventLogger::LogLevel::INFO);
    break;

  default:
    Serial.print("Inverter power target was ");
    Serial.println(inverterPowerMode);
    break;
  }
}

void controlCamera()
{
  if (cameraState == cameraTarget)
    return;

  cameraState = cameraTarget;

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

void onEspNowDataReceived(const uint8_t *mac_addr, const uint8_t *incomingData, int len)
{
  if (len != sizeof(InverterMessage))
  {
    eventLog.log("ESP-NOW: Fel storlek på mottagen data, ignorerar", EventLogger::LogLevel::DATA);
    return;
  }

  InverterMessage msg;
  memcpy(&msg, incomingData, sizeof(InverterMessage));
  receivedInverterCommand = msg.action;
  newInverterCommandAvailable = true;
}