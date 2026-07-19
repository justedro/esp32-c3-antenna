#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <RCSwitch.h>
#include <esp_wifi.h>
#include <string.h>
#include "secrets.h"

// STX882 DATA — TX only
constexpr uint8_t PIN_TX = 4;
constexpr uint8_t PIN_LED = 8;  // Super Mini onboard LED (active-low)

// SoftAP
constexpr char AP_SSID[] = "novy-hood";
constexpr char AP_PASS[] = "novyhood1";  // WPA2 — some phones hide open APs
constexpr uint8_t AP_CHANNEL = 1;

// Novy 433.92 MHz codes (from renedis/ESP32_Novy_Controller)
static const char* const NOVY_DEVICE_CODE[] = {
  "0101", "1001", "0001", "1110", "0110",
  "1010", "0010", "1100", "0100", "1000",
};
static const char* NOVY_PREFIX = "0101";
static const char* NOVY_COMMAND_LIGHT = "0111010001";
static const char* NOVY_COMMAND_POWER = "0111010011";
static const char* NOVY_COMMAND_PLUS = "0101";
static const char* NOVY_COMMAND_MINUS = "0110";
static const char* NOVY_COMMAND_NOVY = "0100";

enum class NovyCmd : uint8_t {
  Light,
  Brightness,
  BrightnessLow,
  Power,
  Plus,
  Minus,
  Novy,
};

RCSwitch transmitter;
WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Optimistic UI only — RF is one-way
bool lightOn = false;
bool mqttDiscoverySent = false;
uint32_t lastMqttAttemptMs = 0;

void sendNovy(NovyCmd cmd);  // defined below
bool executeCmd(String cmd);
const char* wifiPsName(wifi_ps_type_t ps);
void applyWifiPowerSave(bool enableModemSleep);

void sendNovy(NovyCmd cmd) {
  const char* command = NOVY_COMMAND_LIGHT;
  int repeats = 3;
  const char* name = "LIGHT";

  switch (cmd) {
    case NovyCmd::Light:
      command = NOVY_COMMAND_LIGHT;
      repeats = 2;  // more than 2 → brightness change
      name = "LIGHT";
      break;
    case NovyCmd::Brightness:
      command = NOVY_COMMAND_LIGHT;
      repeats = 4;
      name = "BRIGHTNESS";
      break;
    case NovyCmd::BrightnessLow:
      command = NOVY_COMMAND_LIGHT;
      repeats = 10;
      name = "BRIGHTNESS_LOW";
      break;
    case NovyCmd::Power:
      command = NOVY_COMMAND_POWER;
      name = "POWER";
      break;
    case NovyCmd::Plus:
      command = NOVY_COMMAND_PLUS;
      name = "PLUS";
      break;
    case NovyCmd::Minus:
      command = NOVY_COMMAND_MINUS;
      name = "MINUS";
      break;
    case NovyCmd::Novy:
      command = NOVY_COMMAND_NOVY;
      name = "NOVY";
      break;
  }

  const String rfCode =
      String(NOVY_DEVICE_CODE[NOVY_CHANNEL]) + NOVY_PREFIX + command;

  // Drive DATA only while sending — keeps STX882 quiet for WiFi coexistence
  pinMode(PIN_TX, OUTPUT);
  digitalWrite(PIN_TX, LOW);
  transmitter.enableTransmit(PIN_TX);
  transmitter.setPulseLength(350);
  transmitter.setProtocol(12);

  for (int i = 0; i < repeats; i++) {
    transmitter.send(rfCode.c_str());
    delay(50);
  }

  transmitter.disableTransmit();
  digitalWrite(PIN_TX, LOW);

  Serial.print(F("[RF] "));
  Serial.print(name);
  Serial.print(' ');
  Serial.print(rfCode);
  Serial.print(F(" x"));
  Serial.println(repeats);
}

String mqttTopic(const char* suffix) {
  String t = MQTT_PREFIX;
  t += '/';
  t += suffix;
  return t;
}

bool executeCmd(String cmd) {
  cmd.toLowerCase();
  cmd.trim();

  if (cmd == "light") {
    lightOn = !lightOn;
    sendNovy(NovyCmd::Light);
  } else if (cmd == "brightness") {
    sendNovy(NovyCmd::Brightness);
  } else if (cmd == "brightnesslow") {
    sendNovy(NovyCmd::BrightnessLow);
  } else if (cmd == "power") {
    sendNovy(NovyCmd::Power);
  } else if (cmd == "plus") {
    sendNovy(NovyCmd::Plus);
  } else if (cmd == "minus") {
    sendNovy(NovyCmd::Minus);
  } else if (cmd == "novy") {
    sendNovy(NovyCmd::Novy);
  } else {
    return false;
  }

  if (mqtt.connected()) {
    mqtt.publish(mqttTopic("status/last_cmd").c_str(), cmd.c_str(), false);
  }
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    msg += static_cast<char>(payload[i]);
  }
  msg.trim();

  const String topicStr = topic;
  Serial.print(F("[MQTT] "));
  Serial.print(topicStr);
  Serial.print(F(" => "));
  Serial.println(msg);

  // novy/cmd  payload: light|power|plus|...
  if (topicStr == mqttTopic("cmd")) {
    if (!executeCmd(msg)) {
      Serial.println(F("[MQTT] unknown cmd"));
    }
    return;
  }

  // novy/button/<name>  payload: ON | PRESS | anything non-empty
  const String buttonPrefix = mqttTopic("button/");
  if (topicStr.startsWith(buttonPrefix) && msg.length() > 0) {
    String name = topicStr.substring(buttonPrefix.length());
    if (!executeCmd(name)) {
      Serial.println(F("[MQTT] unknown button"));
    }
  }
}

void publishHaDiscovery() {
  if (!mqtt.connected() || mqttDiscoverySent) {
    return;
  }

  static const char* buttons[] = {
    "light", "power", "plus", "minus", "novy", "brightness", "brightnesslow",
  };

  for (const char* btn : buttons) {
    String discTopic = "homeassistant/button/";
    discTopic += MQTT_CLIENT_ID;
    discTopic += '_';
    discTopic += btn;
    discTopic += "/config";

    String cmdTopic = mqttTopic("button/");
    cmdTopic += btn;

    // Compact HA MQTT discovery JSON
    String payload = "{\"name\":\"";
    payload += btn;
    payload += "\",\"unique_id\":\"";
    payload += MQTT_CLIENT_ID;
    payload += '_';
    payload += btn;
    payload += "\",\"command_topic\":\"";
    payload += cmdTopic;
    payload += "\",\"payload_press\":\"PRESS\",\"device\":{\"identifiers\":[\"";
    payload += MQTT_CLIENT_ID;
    payload += "\"],\"name\":\"Novy Hood\",\"model\":\"ESP32-C3 STX882\",\"manufacturer\":\"local\"}}";

    mqtt.publish(discTopic.c_str(), payload.c_str(), true);
  }

  mqttDiscoverySent = true;
  Serial.println(F("[MQTT] HA discovery published"));
}

bool mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (mqtt.connected()) {
    return true;
  }

  Serial.print(F("[MQTT] connecting "));
  Serial.print(MQTT_HOST);
  Serial.print(':');
  Serial.println(MQTT_PORT);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  // Default 15s is fine with MIN_MODEM; keep headroom vs DTIM wake latency.
  mqtt.setKeepAlive(30);

  const String lwt = mqttTopic("status");
  bool ok = false;
  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD, lwt.c_str(), 0, true, "offline");
  } else {
    ok = mqtt.connect(MQTT_CLIENT_ID, lwt.c_str(), 0, true, "offline");
  }

  if (!ok) {
    Serial.print(F("[MQTT] failed rc="));
    Serial.println(mqtt.state());
    return false;
  }

  mqtt.publish(lwt.c_str(), "online", true);
  mqtt.subscribe(mqttTopic("cmd").c_str());
  mqtt.subscribe(mqttTopic("button/#").c_str());
  publishHaDiscovery();
  Serial.println(F("[MQTT] connected"));
  return true;
}

void mqttLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  if (millis() - lastMqttAttemptMs < 5000) {
    return;
  }
  lastMqttAttemptMs = millis();
  mqttDiscoverySent = false;
  mqttConnect();
}

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Novy</title>
<style>
  :root {
    --bg: #12141a;
    --card: #1c1f28;
    --text: #eef0f4;
    --muted: #8b93a7;
    --accent: #3d8bfd;
    --track: #2a2f3c;
    --btn: #2a3142;
    --btn-hover: #364057;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    min-height: 100vh;
    display: grid;
    place-items: center;
    font-family: "Segoe UI", system-ui, sans-serif;
    background: radial-gradient(1200px 600px at 20% 0%, #1a2233, var(--bg));
    color: var(--text);
  }
  main {
    width: min(400px, 92vw);
    background: var(--card);
    border-radius: 20px;
    padding: 1.5rem 1.4rem 1.6rem;
    box-shadow: 0 18px 50px rgba(0,0,0,.35);
  }
  h1 { margin: 0 0 .25rem; font-size: 1.35rem; }
  .sub { margin: 0 0 1.2rem; color: var(--muted); font-size: .92rem; }
  h2 {
    margin: 1.1rem 0 .55rem;
    font-size: .75rem;
    text-transform: uppercase;
    letter-spacing: .08em;
    color: var(--muted);
    font-weight: 600;
  }
  .row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
    padding: .5rem 0 .85rem;
  }
  .label { font-weight: 600; }
  .switch { position: relative; width: 56px; height: 32px; flex-shrink: 0; }
  .switch input { opacity: 0; width: 0; height: 0; }
  .slider {
    position: absolute; inset: 0;
    background: var(--track);
    border-radius: 999px;
    cursor: pointer;
    transition: .2s;
  }
  .slider:before {
    content: "";
    position: absolute;
    width: 26px; height: 26px;
    left: 3px; top: 3px;
    background: #fff;
    border-radius: 50%;
    transition: .2s;
  }
  input:checked + .slider { background: var(--accent); }
  input:checked + .slider:before { transform: translateX(24px); }
  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: .65rem;
  }
  .grid .wide { grid-column: 1 / -1; }
  button {
    appearance: none;
    border: 0;
    border-radius: 12px;
    background: var(--btn);
    color: var(--text);
    font: inherit;
    font-weight: 600;
    padding: .85rem .7rem;
    cursor: pointer;
    transition: background .15s, transform .1s;
  }
  button:hover { background: var(--btn-hover); }
  button:active { transform: scale(.98); }
  button:disabled { opacity: .55; cursor: wait; }
  .status {
    margin-top: 1rem;
    font-size: .85rem;
    color: var(--muted);
    min-height: 1.2em;
  }
</style>
</head>
<body>
<main>
  <h1>Novy hood</h1>
  <p class="sub">433 MHz · channel %CHANNEL% · %NET%</p>

  <h2>Light</h2>
  <div class="row">
    <span class="label">On / Off</span>
    <label class="switch">
      <input type="checkbox" id="light" %CHECKED%>
      <span class="slider"></span>
    </label>
  </div>
  <div class="grid">
    <button type="button" data-cmd="brightness">Brightness+</button>
    <button type="button" data-cmd="brightnesslow">Brightness−</button>
  </div>

  <h2>Ventilation</h2>
  <div class="grid">
    <button type="button" class="wide" data-cmd="power">Power</button>
    <button type="button" data-cmd="plus">Speed +</button>
    <button type="button" data-cmd="minus">Speed −</button>
    <button type="button" class="wide" data-cmd="novy">Novy</button>
  </div>

  <div class="status" id="status"></div>
</main>
<script>
const status = document.getElementById('status');
const light = document.getElementById('light');
let busy = false;

async function send(cmd) {
  if (busy) return;
  busy = true;
  document.querySelectorAll('button').forEach(b => b.disabled = true);
  status.textContent = 'Sending ' + cmd + '…';
  try {
    const r = await fetch('/api/cmd', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ cmd })
    });
    const j = await r.json();
    status.textContent = j.ok ? ('RF sent · ' + j.cmd) : ('Error · ' + (j.error || ''));
    if (j.cmd === 'light' && typeof j.light === 'boolean') light.checked = j.light;
  } catch (e) {
    status.textContent = 'Request failed';
  } finally {
    busy = false;
    document.querySelectorAll('button').forEach(b => b.disabled = false);
  }
}

light.addEventListener('change', async () => {
  await send('light');
});

document.querySelectorAll('button[data-cmd]').forEach(btn => {
  btn.addEventListener('click', () => send(btn.dataset.cmd));
});
</script>
</body>
</html>
)HTML";

void handleRoot() {
  String html = FPSTR(INDEX_HTML);
  html.replace("%CHANNEL%", String(NOVY_CHANNEL));
  html.replace("%CHECKED%", lightOn ? "checked" : "");
  if (WiFi.status() == WL_CONNECTED) {
    html.replace("%NET%", String("STA ") + WiFi.localIP().toString());
  } else {
    html.replace("%NET%", "SoftAP 192.168.4.1");
  }
  server.send(200, "text/html", html);
}

void handleCmdApi() {
  String body = server.arg("plain");
  String cmd;

  if (server.hasArg("cmd")) {
    cmd = server.arg("cmd");
  } else {
    const int key = body.indexOf("\"cmd\"");
    if (key >= 0) {
      const int colon = body.indexOf(':', key);
      const int q1 = body.indexOf('"', colon + 1);
      const int q2 = body.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 > q1) {
        cmd = body.substring(q1 + 1, q2);
      }
    }
  }

  cmd.toLowerCase();

  if (!executeCmd(cmd)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown cmd\"}");
    return;
  }

  String json = "{\"ok\":true,\"cmd\":\"";
  json += cmd;
  json += "\",\"light\":";
  json += lightOn ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

void handleStatus() {
  const bool sta = WiFi.status() == WL_CONNECTED;
  String json = "{\"light\":";
  json += lightOn ? "true" : "false";
  json += ",\"channel\":";
  json += String(NOVY_CHANNEL);
  json += ",\"mode\":\"";
  json += sta ? "sta" : "ap";
  json += "\",\"ip\":\"";
  json += sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  json += "\",\"ssid\":\"";
  json += sta ? WIFI_SSID : AP_SSID;
  json += "\",\"mqtt\":";
  json += mqtt.connected() ? "true" : "false";
  json += ",\"cpu_mhz\":";
  json += String(getCpuFrequencyMhz());
  json += ",\"wifi_ps\":\"";
  {
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (sta) {
      esp_wifi_get_ps(&ps);
    }
    json += wifiPsName(ps);
  }
  json += "\"}";
  server.send(200, "application/json", json);
}

static volatile uint8_t gLastDisconnectReason = 0;
// Modem sleep after assoc saves heat; fall back to PS_NONE if reason 34 returns.
static bool gModemSleepAllowed = true;
static bool gModemSleepActive = false;

const char* wifiReasonName(uint8_t r) {
  switch (r) {
    case 2: return "AUTH_EXPIRE";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 34: return "DISASSOC_LOW_ACK";  // poor channel / missing ACKs
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default: return "?";
  }
}

const char* wifiPsName(wifi_ps_type_t ps) {
  switch (ps) {
    case WIFI_PS_NONE: return "NONE";
    case WIFI_PS_MIN_MODEM: return "MIN_MODEM";
    case WIFI_PS_MAX_MODEM: return "MAX_MODEM";
    default: return "?";
  }
}

void applyWifiPowerSave(bool enableModemSleep) {
  if (enableModemSleep && gModemSleepAllowed) {
    // MIN_MODEM: wake every DTIM — keeps association, AP buffers unicast (MQTT/HTTP).
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    gModemSleepActive = true;
  } else {
    WiFi.setSleep(WIFI_PS_NONE);
    gModemSleepActive = false;
  }
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  Serial.print(F("[PWR] WiFi PS="));
  Serial.print(wifiPsName(ps));
  Serial.print(F(" CPU="));
  Serial.print(getCpuFrequencyMhz());
  Serial.println(F(" MHz"));
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    gLastDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.print(F("\n[WiFi] disconnect reason="));
    Serial.print(gLastDisconnectReason);
    Serial.print(F(" ("));
    Serial.print(wifiReasonName(gLastDisconnectReason));
    Serial.println(')');
    // Modem sleep contributed to missing ACKs during bring-up — disable for session.
    if (gLastDisconnectReason == 34 && gModemSleepActive) {
      gModemSleepAllowed = false;
      applyWifiPowerSave(false);
      Serial.println(F("[PWR] reason 34 → PS_NONE for this session"));
    }
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.print(F("\n[WiFi] got IP "));
    Serial.println(WiFi.localIP());
  } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println(F("\n[WiFi] associated"));
  }
}

bool waitForSta(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    Serial.print('.');
    delay(300);
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

bool tryStaOnce(wifi_power_t power, bool bgOnly, const uint8_t* bssid, uint8_t channel) {
  gLastDisconnectReason = 0;

  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.setHostname("novy-hood");

  esp_wifi_set_ps(WIFI_PS_NONE);
  if (bgOnly) {
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
  } else {
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  }

  WiFi.setTxPower(power);
  // Match Arduino enum (~0.25 dBm units). Keep modest — C3 often fails at max power.
  esp_wifi_set_max_tx_power(60);

  Serial.print(F("TX power enum="));
  Serial.print((int)power);
  Serial.print(F(" bgOnly="));
  Serial.print(bgOnly ? F("yes") : F("no"));
  if (bssid) {
    Serial.print(F(" BSSID ch="));
    Serial.print(channel);
  }
  Serial.println();

  if (bssid) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, channel, bssid, true);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  return waitForSta(18000);
}

bool connectSta() {
  Serial.print(F("STA connecting to \""));
  Serial.print(WIFI_SSID);
  Serial.println('"');
  Serial.println(F("Mitigations: newer core, PS_NONE, lower TX power, 11b/g, BSSID"));

  WiFi.onEvent(onWifiEvent);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Find strongest matching AP
  uint8_t bestBssid[6] = {};
  uint8_t bestCh = 0;
  int32_t bestRssi = -127;
  bool haveBssid = false;

  Serial.println(F("Scanning…"));
  const int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      bestCh = WiFi.channel(i);
      memcpy(bestBssid, WiFi.BSSID(i), 6);
      haveBssid = true;
    }
  }
  WiFi.scanDelete();

  if (haveBssid) {
    Serial.print(F("Best AP RSSI="));
    Serial.print(bestRssi);
    Serial.print(F(" ch="));
    Serial.println(bestCh);
  } else {
    Serial.println(F("SSID not seen in scan — trying anyway"));
  }

  // ESP32-C3: high TX power often causes AUTH_EXPIRE / reason 34. Try low→mid.
  const wifi_power_t powers[] = {
    WIFI_POWER_8_5dBm,
    WIFI_POWER_11dBm,
    WIFI_POWER_15dBm,
    WIFI_POWER_17dBm,
  };

  for (wifi_power_t p : powers) {
    if (haveBssid && tryStaOnce(p, true, bestBssid, bestCh)) {
      goto connected;
    }
    if (tryStaOnce(p, true, nullptr, 0)) {
      goto connected;
    }
  }

  // Last resort: 11n + slightly higher power + BSSID
  if (haveBssid && tryStaOnce(WIFI_POWER_15dBm, false, bestBssid, bestCh)) {
    goto connected;
  }
  if (tryStaOnce(WIFI_POWER_15dBm, false, nullptr, 0)) {
    goto connected;
  }

  Serial.print(F("STA failed, lastReason="));
  Serial.print(gLastDisconnectReason);
  Serial.print(F(" ("));
  Serial.print(wifiReasonName(gLastDisconnectReason));
  Serial.println(')');
  WiFi.disconnect(true, true);
  return false;

connected:
  WiFi.setAutoReconnect(true);
  // Assoc used PS_NONE; enable modem sleep only after the link is up.
  applyWifiPowerSave(true);
  Serial.print(F("STA OK  IP="));
  Serial.print(WiFi.localIP());
  Serial.print(F("  RSSI="));
  Serial.print(WiFi.RSSI());
  Serial.print(F("  TX="));
  Serial.println(WiFi.getTxPower());
  return true;
}

void startSoftAp() {
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP);
  delay(200);

  const IPAddress ip(192, 168, 4, 1);
  const IPAddress gw(192, 168, 4, 1);
  const IPAddress mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gw, mask);

  const bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, false, 4);
  // SoftAP also happier at moderate power on C3
  WiFi.setTxPower(WIFI_POWER_11dBm);
  esp_wifi_set_max_tx_power(44);
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.print(F("softAP()="));
  Serial.println(ok ? F("OK") : F("FAIL"));
  Serial.print(F("SoftAP \""));
  Serial.print(AP_SSID);
  Serial.print(F("\" / "));
  Serial.print(AP_PASS);
  Serial.print(F(" → http://"));
  Serial.println(WiFi.softAPIP());
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println(F("=== Novy hood controller ==="));
  Serial.println(F("platform espressif32@7.0.1 + WiFi reliability tweaks"));
  Serial.println(F("Wiring tip: STX882 VCC -> 5V (not 3V3), GND common,"));
  Serial.println(F("  100nF||100-470uF at module; DATA->GPIO4; antennas apart."));

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Keep RF DATA idle until after WiFi is up (3V3 STX882 loads the rail)
  pinMode(PIN_TX, OUTPUT);
  digitalWrite(PIN_TX, LOW);

  if (!connectSta()) {
    Serial.println(F("Falling back to SoftAP"));
    startSoftAp();
  }

  // 80 MHz is enough for HTTP/MQTT/RF and cuts idle heat vs 160 MHz.
  setCpuFrequencyMhz(80);
  Serial.print(F("[PWR] CPU set to "));
  Serial.print(getCpuFrequencyMhz());
  Serial.println(F(" MHz"));

  // RF ready only after WiFi assoc — reduce boot-time rail stress
  transmitter.disableTransmit();
  digitalWrite(PIN_TX, LOW);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/cmd", HTTP_POST, handleCmdApi);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.begin();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("Web UI → http://"));
    Serial.println(WiFi.localIP());
    mqttConnect();
  } else {
    Serial.println(F("Web UI → join novy-hood → http://192.168.4.1"));
    Serial.println(F("MQTT skipped (STA required)"));
  }
}

void loop() {
  server.handleClient();
  mqttLoop();

  // Yield so FreeRTOS idle can run — busy-spin was a major heat source.
  delay(1);

  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (now - lastMs >= 15000) {
    lastMs = now;
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    if (WiFi.status() == WL_CONNECTED) {
      wifi_ps_type_t ps = WIFI_PS_NONE;
      esp_wifi_get_ps(&ps);
      Serial.print(F("[STA] IP="));
      Serial.print(WiFi.localIP());
      Serial.print(F(" RSSI="));
      Serial.print(WiFi.RSSI());
      Serial.print(F(" PS="));
      Serial.print(wifiPsName(ps));
      Serial.print(F(" CPU="));
      Serial.print(getCpuFrequencyMhz());
      Serial.println(F(" MHz"));
    } else if (WiFi.getMode() & WIFI_AP) {
      Serial.print(F("[AP] stations="));
      Serial.print(WiFi.softAPgetStationNum());
      Serial.print(F(" IP="));
      Serial.println(WiFi.softAPIP());
    }
  }
}
