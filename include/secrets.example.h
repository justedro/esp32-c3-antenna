#pragma once

// >>> Fill WiFi here <<<
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// >>> Fill MQTT here <<<
#define MQTT_HOST     "192.168.1.10"   // broker IP or hostname
#define MQTT_PORT     1883
#define MQTT_USER     ""              // empty if no auth
#define MQTT_PASSWORD ""
#define MQTT_CLIENT_ID "novy-hood"
#define MQTT_PREFIX   "novy"          // topics: novy/cmd , novy/button/<name>

// Novy remote channel index 0..9 (matches physical remote pairing)
#ifndef NOVY_CHANNEL
#define NOVY_CHANNEL 0
#endif
