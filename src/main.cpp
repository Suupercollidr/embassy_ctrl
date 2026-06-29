#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <AsyncMqttClient.h>
#include <time.h>
#include <RTClib.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include "debounce.h"
#include "EventLogger.h"
#include "nvsDebugData.h"
#include "configuration.h"
// #include "dev_configuration.h"

WebServer localWebServer(80);
AsyncMqttClient mqttClient;
InfluxDBClient influxLogClient(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_LOG_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);

EventLogger eventLog(influxLogClient, -1);

Debounce cameraButtonDebounce;
Debounce inverterPowerChangeInterval(INV_RETRY_PERIOD * 1000);
Debounce NTPSyncInterval(NTP_SYNC_INTERVAL * 3600000);
Debounce MQTTReconnect(10000);


enum powerSwitch
{
  OFF,
  ON
};

powerSwitch inverterPowerState = ON;
powerSwitch xmasLightState = OFF;
volatile powerSwitch cameraTarget = ON;
powerSwitch cameraState = ON;

const char *getResetReason(esp_reset_reason_t);

void initWiFi();
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total);
void controlInverter();
void controlLight();
void controlCamera();
void IRAM_ATTR camButtonPush();

void setup()
{

  initNvs();
  String lastEventBeforeReboot = String(readDataFromNvs("lastState"));
  esp_reset_reason_t resetReason = esp_reset_reason();

  Serial.begin(115200);
  Serial.println("====================");
  Serial.println(" System is starting ");
  Serial.println("====================");

  pinMode(RELAY_INV, OUTPUT);
  pinMode(RELAY_LIGHT, OUTPUT);
  pinMode(RELAY_CAM, OUTPUT);
  pinMode(RELAY_AUX, OUTPUT);
  pinMode(LED_CAM_BUTTON, OUTPUT);
  pinMode(CAM_BUTTON, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(CAM_BUTTON), camButtonPush, FALLING);

  // Make sure relay positions match the corresponding power switch
  digitalWrite(RELAY_INV, HIGH);                                  // NC
  digitalWrite(RELAY_LIGHT, (xmasLightState == ON) ? HIGH : LOW); // NO
  digitalWrite(RELAY_CAM, (cameraState == ON) ? LOW : HIGH);      // NC
  digitalWrite(RELAY_AUX, LOW);
  digitalWrite(LED_CAM_BUTTON, LOW);

  initWiFi();

  // Testa om MQTT-brokers IP är nåbar via TCP
  WiFiClient testClient;
  if (testClient.connect(MQTT_HOST, 1883))
  {
    eventLog.log("TCP-anslutning till MQTT-broker lyckades", EventLogger::LogLevel::INFO);
    testClient.stop();
  }
  else
  {
    eventLog.log("TCP-anslutning till MQTT-broker MISSLYCKADES", EventLogger::LogLevel::ERROR);
  }

  timeSync(TIME_ZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);

  // OTA
  localWebServer.on("/", []()
                    { localWebServer.send(200, "text/plain", "Tere tulemast Eesti saatkonda!"); });
  ElegantOTA.setAuth(otaUsername, otaPassword);
  ElegantOTA.begin(&localWebServer);
  localWebServer.begin();
  eventLog.log("Webbserver startad", EventLogger::LogLevel::INFO);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.setCredentials(MQTT_USER, MQTT_PASS);
  mqttClient.setServer(MQTT_HOST, 1883);
  mqttClient.connect();

  if (!influxLogClient.validateConnection())
    eventLog.log(String("Kunde inte ansluta till Influx DB på " + influxLogClient.getServerUrl() + "\nFelmeddelande:\n" + influxLogClient.getLastErrorMessage() + "\n"), EventLogger::LogLevel::ERROR);
  float setupTime = millis() / 1000.0f;

  eventLog.log(String("Systemet startat. Uppstarten tog " + String(setupTime) + " s."), EventLogger::LogLevel::INFO);
  eventLog.log(String("Senaste återställning: " + String(getResetReason(resetReason)) + " (" + String(resetReason) + ")"), EventLogger::LogLevel::INFO);
  eventLog.log(String("Senaste åtgärd: " + lastEventBeforeReboot), EventLogger::LogLevel::INFO);

  storeDataToNvs("lastState", "Setup end");
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    WiFi.reconnect();
    eventLog.log("Återansluter till WiFi", EventLogger::LogLevel::INFO);
    delay(500);

    if (WiFi.status() == WL_CONNECTED && !mqttClient.connected())
      mqttClient.connect();
  }

  if (MQTTReconnect.ready())
  {
    eventLog.log("Försöker återansluta till MQTT...", EventLogger::LogLevel::INFO);
    mqttClient.connect();
  }

  localWebServer.handleClient();
  ElegantOTA.loop();

  controlCamera();

  if (NTPSyncInterval.ready())
    timeSync(TIME_ZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);

  yield();
}

void initWiFi() // Connect to WiFi
{
  storeDataToNvs("lastState", "initWiFi");
  eventLog.log(String("Ansluter till WiFi " + String(ssid)), EventLogger::LogLevel::INFO);
  eventLog.log(String("MAC-adress: " + WiFi.macAddress()), EventLogger::LogLevel::INFO);

  WiFi.begin(ssid, password, 6);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(100);

    Serial.print(".");
  }
  IPAddress myIp = WiFi.localIP();
  const String myIpString = String(myIp[0]) + "." +
                            String(myIp[1]) + "." +
                            String(myIp[2]) + "." +
                            String(myIp[3]);

  IPAddress gwIp = WiFi.gatewayIP();
  const String gwIpString = String(gwIp[0]) + "." +
                            String(gwIp[1]) + "." +
                            String(gwIp[2]) + "." +
                            String(gwIp[3]);

  Serial.println();
  eventLog.log("Ansluten till WiFi " + String(ssid), EventLogger::LogLevel::INFO);
  Serial.flush();
  Serial.println("\t\t\t\t\tKanal    \t" + WiFi.channel());
  Serial.println("\t\t\t\t\tIP-adress\t" + myIpString);
  Serial.println("\t\t\t\t\tGateway  \t" + gwIpString);

  /*Point netStat("Network");
  netStat.addField("Channel", WiFi.channel());
  netStat.addField("Hostname", WiFi.getHostname());
  netStat.addField("IP address", myIpString);
  netStat.addField("Gateway", gwIpString);
  netStat.addField("MAC address", WiFi.macAddress());

  influxClient.writePoint(netStat);
  */
}

void onMqttConnect(bool sessionPresent)
{
  uint16_t packetId = mqttClient.subscribe(camera_command_topic, 1);

  eventLog.log("MQTT: Ansluten till broker", EventLogger::LogLevel::INFO);
  eventLog.log(String("MQTT: Prenumererar på " + String(camera_command_topic)), EventLogger::LogLevel::INFO);
  eventLog.log(String("MQTT: Subscribe packet ID " + String(packetId)), EventLogger::LogLevel::INFO);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  String message = "Frånkopplad från MQTT-broker p.g.a.: ";
  message += static_cast<int>(reason);
  eventLog.log(message, EventLogger::LogLevel::WARNING);
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
    eventLog.log("MQTT: Kamera ON-kommando mottaget", EventLogger::LogLevel::INFO);
  }
  else if (message == "OFF")
  {
    cameraTarget = OFF;
    eventLog.log("MQTT: Kamera OFF-kommando mottaget", EventLogger::LogLevel::INFO);
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
    digitalWrite(LED_CAM_BUTTON, LOW); // Green light off when cam on
    digitalWrite(RELAY_CAM, LOW);      // Relay is NC
    mqttClient.publish(camera_state_topic, 1, true, "ON");
    eventLog.log("Camera turned on", EventLogger::LogLevel::INFO);
    break;

  case OFF:
    digitalWrite(LED_CAM_BUTTON, HIGH); // Green light indicates cam is OFF
    digitalWrite(RELAY_CAM, HIGH);      // Relay is NC
    mqttClient.publish(camera_state_topic, 1, true, "OFF");
    eventLog.log("Camera turned off", EventLogger::LogLevel::INFO);
    break;

  default:
    Serial.print("Camera target was ");
    Serial.println(cameraTarget);
    break;
  }
}

void IRAM_ATTR camButtonPush()
{
  if (cameraButtonDebounce.ready())
    cameraTarget = (cameraTarget == ON) ? OFF : ON;
}