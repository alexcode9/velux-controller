#include <ESP8266WiFi.h>
#include <espMqttClient.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "remotes.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and set Wi-Fi and MQTT credentials."
#endif

namespace {

constexpr size_t kIdLen = 9;
constexpr size_t kClientIdLen = 24;
constexpr size_t kTopicLen = 96;
constexpr size_t kPayloadLen = 768;

char chipId_[kIdLen];
char clientId_[kClientIdLen];
char availabilityTopic_[kTopicLen];
char commandTopic_[kRemoteCount][kTopicLen];
char discoveryTopic_[kRemoteCount][kTopicLen];
char discoveryPayload_[kPayloadLen];

WiFiEventHandler wifiConnectHandler_;
WiFiEventHandler wifiDisconnectHandler_;
espMqttClient mqttClient_;

bool reconnectMqtt_ = false;
bool discoveryReady_ = false;
bool commandsSubscribed_ = false;
uint32_t lastWifiAttemptMs_ = 0;
uint32_t lastMqttAttemptMs_ = 0;

bool formatInto(char* dest, size_t len, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int written = vsnprintf(dest, len, fmt, args);
  va_end(args);
  return written >= 0 && static_cast<size_t>(written) < len;
}

bool payloadEquals(const uint8_t* payload, size_t len, const char* expected) {
  const size_t expectedLen = strlen(expected);
  return len == expectedLen && memcmp(payload, expected, len) == 0;
}

bool publishRetained(const char* topic, const char* payload) {
  // QoS 0. Discovery and availability are retained. Command topics are never
  // published by this firmware.
  return mqttClient_.publish(topic, 0, true, payload) != 0;
}

bool publishCoverDiscovery(uint8_t remote) {
  const unsigned number = static_cast<unsigned>(remote) + 1;
  if (!formatInto(
          discoveryPayload_,
          sizeof(discoveryPayload_),
          "{\"name\":\"%s\","
          "\"unique_id\":\"velux_%s_%u\","
          "\"command_topic\":\"%s\","
          "\"payload_open\":\"%s\","
          "\"payload_close\":\"%s\","
          "\"payload_stop\":\"%s\","
          "\"optimistic\":true,"
          "\"availability_topic\":\"%s\","
          "\"payload_available\":\"%s\","
          "\"payload_not_available\":\"%s\","
          "\"device\":{"
          "\"identifiers\":[\"velux_%s\"],"
          "\"name\":\"VELUX Controller\","
          "\"manufacturer\":\"Custom\","
          "\"model\":\"NodeMCU V3\""
          "}}",
          kCoverNames[remote],
          chipId_,
          number,
          commandTopic_[remote],
          kPayloadOpen,
          kPayloadClose,
          kPayloadStop,
          availabilityTopic_,
          kPayloadOnline,
          kPayloadOffline,
          chipId_)) {
    Serial.println("discovery payload does not fit");
    return false;
  }
  return publishRetained(discoveryTopic_[remote], discoveryPayload_);
}

bool subscribeCommands() {
  const uint16_t birth = mqttClient_.subscribe(kHaBirthTopic, 0);
  const uint16_t first = mqttClient_.subscribe(commandTopic_[0], 0);
  const uint16_t second = mqttClient_.subscribe(commandTopic_[1], 0);
  if (birth == 0 || first == 0 || second == 0) {
    Serial.println("MQTT subscribe failed");
    return false;
  }
  commandsSubscribed_ = true;
  Serial.println("subscribed to birth and command topics");
  return true;
}

// Availability and both discovery messages first. Command subscriptions stay
// closed until that publish succeeds, including on a later retry.
bool publishSetup() {
  if (!publishRetained(availabilityTopic_, kPayloadOnline)) {
    Serial.println("availability publish failed");
    discoveryReady_ = false;
    lastMqttAttemptMs_ = millis();
    return false;
  }
  if (!publishCoverDiscovery(0) || !publishCoverDiscovery(1)) {
    Serial.println("discovery publish failed");
    discoveryReady_ = false;
    lastMqttAttemptMs_ = millis();
    return false;
  }
  if (!commandsSubscribed_ && !subscribeCommands()) {
    discoveryReady_ = false;
    lastMqttAttemptMs_ = millis();
    return false;
  }
  discoveryReady_ = true;
  return true;
}

void onMqttMessage(const espMqttClientTypes::MessageProperties& properties,
                   const char* topic,
                   const uint8_t* payload,
                   size_t len,
                   size_t index,
                   size_t total) {
  (void)properties;
  if (index != 0 || len != total) {
    Serial.println("ignored fragmented MQTT payload");
    return;
  }

  if (strcmp(topic, kHaBirthTopic) == 0) {
    if (payloadEquals(payload, len, kHaBirthPayload)) {
      Serial.println("Home Assistant birth; republishing discovery");
      publishSetup();
    }
    return;
  }

  if (!discoveryReady_) {
    return;
  }

  for (uint8_t remote = 0; remote < kRemoteCount; ++remote) {
    if (strcmp(topic, commandTopic_[remote]) != 0) {
      continue;
    }
    if (payloadEquals(payload, len, kPayloadOpen)) {
      remotesEnqueue(remote, RemoteAction::Open);
    } else if (payloadEquals(payload, len, kPayloadClose)) {
      remotesEnqueue(remote, RemoteAction::Close);
    } else if (payloadEquals(payload, len, kPayloadStop)) {
      remotesEnqueue(remote, RemoteAction::Stop);
    } else {
      Serial.printf("ignored payload on remote %u\n", remote + 1);
    }
    return;
  }
}

void onMqttConnect(bool sessionPresent) {
  Serial.printf("MQTT connected, session present: %d\n", sessionPresent ? 1 : 0);
  reconnectMqtt_ = false;
  commandsSubscribed_ = false;
  if (!publishSetup()) {
    Serial.println("MQTT setup incomplete; will retry");
  }
}

void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
  Serial.printf("MQTT disconnected: %u\n", static_cast<uint8_t>(reason));
  discoveryReady_ = false;
  commandsSubscribed_ = false;
  if (WiFi.isConnected()) {
    reconnectMqtt_ = true;
    lastMqttAttemptMs_ = millis();
  }
}

void onWiFiConnect(const WiFiEventStationModeGotIP& event) {
  (void)event;
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  Serial.print("Wi-Fi connected, IP ");
  Serial.println(WiFi.localIP());
  reconnectMqtt_ = true;
  lastMqttAttemptMs_ = millis() - kMqttRetryMs;
}

void onWiFiDisconnect(const WiFiEventStationModeDisconnected& event) {
  (void)event;
  Serial.println("Wi-Fi disconnected");
  reconnectMqtt_ = false;
}

bool buildIdentity() {
  bool ok = true;
  ok &= formatInto(chipId_, sizeof(chipId_), "%08lx", static_cast<unsigned long>(ESP.getChipId()));
  ok &= formatInto(clientId_, sizeof(clientId_), "velux-%s", chipId_);
  ok &= formatInto(availabilityTopic_,
                   sizeof(availabilityTopic_),
                   "%s/%s/availability",
                   kMqttTopicPrefix,
                   chipId_);
  for (uint8_t remote = 0; remote < kRemoteCount; ++remote) {
    const unsigned number = static_cast<unsigned>(remote) + 1;
    ok &= formatInto(commandTopic_[remote],
                     sizeof(commandTopic_[remote]),
                     "%s/%s/%u/set",
                     kMqttTopicPrefix,
                     chipId_,
                     number);
    ok &= formatInto(discoveryTopic_[remote],
                     sizeof(discoveryTopic_[remote]),
                     "%s/cover/velux_%s_%u/config",
                     kHaDiscoveryPrefix,
                     chipId_,
                     number);
  }
  return ok;
}

void serviceWifi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  if (now - lastWifiAttemptMs_ < kWifiRetryMs) {
    return;
  }
  lastWifiAttemptMs_ = now;
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void serviceMqtt(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  // connect() blocks for one TCP attempt on ESP8266. Never start that attempt
  // while a button is held, or the pulse would run long.
  if (remotesHolding()) {
    return;
  }

  if (!mqttClient_.connected()) {
    if (!reconnectMqtt_ || now - lastMqttAttemptMs_ < kMqttRetryMs) {
      return;
    }
    Serial.println("Connecting to MQTT...");
    const bool started = mqttClient_.connect();
    lastMqttAttemptMs_ = millis();
    if (started) {
      reconnectMqtt_ = false;
    } else {
      reconnectMqtt_ = true;
      Serial.println("MQTT connect failed");
    }
    return;
  }

  if (!discoveryReady_ && now - lastMqttAttemptMs_ >= kMqttRetryMs) {
    lastMqttAttemptMs_ = now;
    publishSetup();
  }
}

}  // namespace

void setup() {
  remotesBegin();

  Serial.begin(115200);
  Serial.println();
  Serial.println("VELUX controller");

  if (!buildIdentity()) {
    Serial.println("topic buffer too small; shorten the MQTT prefix or cover names");
  }
  Serial.printf("chip %s\n", chipId_);
  Serial.printf("commands %s | %s\n", commandTopic_[0], commandTopic_[1]);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  wifiConnectHandler_ = WiFi.onStationModeGotIP(onWiFiConnect);
  wifiDisconnectHandler_ = WiFi.onStationModeDisconnected(onWiFiDisconnect);

  mqttClient_.onConnect(onMqttConnect);
  mqttClient_.onDisconnect(onMqttDisconnect);
  mqttClient_.onMessage(onMqttMessage);
  mqttClient_.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient_.setClientId(clientId_);
  mqttClient_.setCleanSession(true);
  // Pointers passed to the client must outlive it. These buffers are static.
  mqttClient_.setWill(availabilityTopic_, 0, true, kPayloadOffline);
  if (MQTT_USER[0] != '\0') {
    mqttClient_.setCredentials(MQTT_USER, MQTT_PASSWORD);
  }

  lastWifiAttemptMs_ = millis() - kWifiRetryMs;
}

void loop() {
  const uint32_t now = millis();
  remotesUpdate(now);
  mqttClient_.loop();
  // A command received above starts or preempts before any reconnect attempt.
  remotesUpdate(millis());
  serviceWifi(millis());
  serviceMqtt(millis());
}
