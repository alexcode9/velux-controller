#pragma once

#include <Arduino.h>

// NodeMCU labels and ESP8266 GPIO numbers. All six are normal I/O pins.
// GPIO0, GPIO2, GPIO15, and the UART pins (GPIO1 TX, GPIO3 RX) are not used.
//
// Remote 1: Up D1/GPIO5, Stop D2/GPIO4, Down D5/GPIO14
// Remote 2: Up D6/GPIO12, Stop D0/GPIO16, Down D7/GPIO13

constexpr uint8_t kRemoteCount = 2;

constexpr uint8_t kRemoteUpPin[kRemoteCount] = {D1, D6};
constexpr uint8_t kRemoteStopPin[kRemoteCount] = {D2, D0};
constexpr uint8_t kRemoteDownPin[kRemoteCount] = {D5, D7};

constexpr bool pinAllowedAtBoot(uint8_t pin) {
  return pin != 0 && pin != 1 && pin != 2 && pin != 3 && pin != 15;
}

static_assert(pinAllowedAtBoot(kRemoteUpPin[0]), "remote 1 up pin");
static_assert(pinAllowedAtBoot(kRemoteStopPin[0]), "remote 1 stop pin");
static_assert(pinAllowedAtBoot(kRemoteDownPin[0]), "remote 1 down pin");
static_assert(pinAllowedAtBoot(kRemoteUpPin[1]), "remote 2 up pin");
static_assert(pinAllowedAtBoot(kRemoteStopPin[1]), "remote 2 stop pin");
static_assert(pinAllowedAtBoot(kRemoteDownPin[1]), "remote 2 down pin");
static_assert(kRemoteStopPin[1] == 16, "remote 2 stop is GPIO16");

// How long a button signal is held low, then the pause before the next
// transmission on either remote. Both are milliseconds.
constexpr uint32_t kButtonPulseMs = 200;
constexpr uint32_t kInterCommandGapMs = 300;

// At most one pending command is kept per remote. Capacity is still a hard
// bound; see remotes.cpp for which command is dropped when it is exceeded.
constexpr uint8_t kCommandQueueCapacity = 8;

// Cover names are inserted into the discovery JSON. Do not put quotes in them.
constexpr char kMqttTopicPrefix[] = "velux";
constexpr const char* kCoverNames[kRemoteCount] = {"Velux 1", "Velux 2"};

// Home Assistant default birth message. Republish discovery when this arrives.
constexpr char kHaDiscoveryPrefix[] = "homeassistant";
constexpr char kHaBirthTopic[] = "homeassistant/status";
constexpr char kHaBirthPayload[] = "online";

constexpr char kPayloadOpen[] = "OPEN";
constexpr char kPayloadClose[] = "CLOSE";
constexpr char kPayloadStop[] = "STOP";
constexpr char kPayloadOnline[] = "online";
constexpr char kPayloadOffline[] = "offline";

// Minimum time between Wi-Fi begin() retries and between MQTT connect attempts.
constexpr uint32_t kWifiRetryMs = 5000;
constexpr uint32_t kMqttRetryMs = 5000;
