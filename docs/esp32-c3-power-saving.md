# ESP32-C3 — Power saving while staying on WiFi

Goal for this project: reduce MCU heat / idle power on an **always-on** STA controller (HTTP + MQTT PubSubClient + occasional 433 MHz TX) **without** dropping association or becoming unresponsive to remote commands.

Related: [esp32-c3-wifi-reliability.md](./esp32-c3-wifi-reliability.md) (reason 34 / low TX / `WIFI_PS_NONE` bring-up).

## Platform stack (this repo)

| Layer | What `espressif32@7.0.1` + `framework = arduino` actually gives |
|-------|------------------------------------------------------------------|
| PlatformIO platform | [espressif32 v7.0.1](https://github.com/platformio/platform-espressif32/releases/tag/v7.0.1) |
| Arduino framework | **Arduino-ESP32 v2.0.17** (based on **ESP-IDF v4.4.7**) |
| Pure IDF (if selected) | ESP-IDF **v6.0.1** (not used by this Arduino env) |

**Important:** Official PlatformIO `espressif32@7.0.1` does **not** ship Arduino-ESP32 3.x. Arduino 3.x / IDF 5.x needs a community platform (e.g. [pioarduino](https://github.com/pioarduino/platform-espressif32)). The WiFi power-save **APIs below already exist in 2.0.17**; the IDF semantics are the same across 4.4 → 5.x.

Board: ESP32-C3 Super Mini, `board_build.f_cpu = 160000000L`.

---

## 1. Modem sleep: `WIFI_PS_NONE` vs `MIN_MODEM` vs `MAX_MODEM`

Espressif’s STA “Modem-sleep” is the IEEE 802.11 legacy power-save mode: between wakes, **RF / PHY / BB are powered down**; the station **keeps association** with the AP. The AP buffers unicast traffic and advertises it in TIM/DTIM beacons.

Primary sources:

- [ESP32-C3 Wi-Fi Power-saving Mode (IDF v5.1)](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32c3/api-guides/wifi.html#esp32-c3-wi-fi-power-saving-mode) — same text in [v4.4.7](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32c3/api-guides/wifi.html)
- [Low power mode in Wi-Fi scenarios (ESP32-C3)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html)
- Enum + `listen_interval` in `wifi_ps_type_t` / `wifi_sta_config_t` ([esp_wifi_types](https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_wifi_types_generic.h))

| Mode | Behavior | Latency for downlink | Power | Reliability notes |
|------|----------|----------------------|-------|-------------------|
| **`WIFI_PS_NONE`** | RF stays available; no modem sleep | **Minimum** (real-time RX) | Highest; most heat | Best for flaky RF / reason 34 bring-up |
| **`WIFI_PS_MIN_MODEM`** (default in IDF) | Wake every **DTIM** to receive beacons | Up to **one DTIM period** (AP-controlled; often ~100–300 ms) | Moderate | Broadcast after DTIM is not missed; Espressif’s recommended modem mode |
| **`WIFI_PS_MAX_MODEM`** | Wake every **`listen_interval`** (beacon intervals) | Up to listen interval (can exceed DTIM) | Lowest among modem modes | May **miss DTIM / broadcast**; configure `listen_interval` before connect |

Official quotes (paraphrased from the Wi-Fi driver guide):

- Modem-sleep works in **station-only** mode after the STA is connected; connection is **maintained**.
- Call `esp_wifi_set_ps(...)` **after** `esp_wifi_init()`. Sleep starts when associated; stops when disconnected.
- Default is **`WIFI_PS_MIN_MODEM`**.
- With modem sleep enabled, RX delay can be as long as the DTIM cycle (`MIN`) or listen interval (`MAX`).

### Measured currents (Espressif shielded-box averages, ESP32-C3)

From [Low power Wi-Fi scenarios](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html):

| Mode | DTIM1 | DTIM3 | DTIM10 | Wi-Fi connection |
|------|-------|-------|--------|------------------|
| Modem-sleep | 21.47 mA | 20.82 mA | 20.67 mA | Maintain |
| Modem-sleep + DFS | 11.35 mA | 10.71 mA | 10.32 mA | Maintain |
| Auto Light-sleep | 1.4 mA | 0.62 mA | 0.31 mA | Maintain |
| Deep-sleep | — | — | — | **Disconnect** (~4.8 µA avg) |

Datasheet modem-sleep CPU contribution ([ESP32-C3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf) Table 5-8; Wi-Fi clock-gated):

| CPU | Idle (periph clocks off) | Running (periph clocks off) |
|-----|--------------------------|-----------------------------|
| 160 MHz | 16 mA | 23 mA |
| 80 MHz | 13 mA | 17 mA |

So for heat: **MIN_MODEM + 80 MHz** is a meaningful step down from **PS_NONE + 160 MHz**, without giving up association.

---

## 2. Arduino / IDF APIs available here

### Arduino-ESP32 2.0.17 (this project)

From [`WiFiGeneric.h` / `.cpp` @ 2.0.17](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/WiFi/src/WiFiGeneric.h):

```cpp
bool setSleep(bool enabled);              // true → WIFI_PS_MIN_MODEM, false → WIFI_PS_NONE
bool setSleep(wifi_ps_type_t sleepType);  // NONE / MIN_MODEM / MAX_MODEM
wifi_ps_type_t getSleep();
```

Implementation: `setSleep` stores the mode and, if STA is up, calls `esp_wifi_set_ps()`. Default `_sleepEnabled` is **`WIFI_PS_MIN_MODEM`** on ESP32-C3 (only ESP32-S2 defaults to `NONE`).

Equivalent IDF calls (always available via `#include "esp_wifi.h"`):

```cpp
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *type);
```

For `MAX_MODEM`, set `wifi_config_t.sta.listen_interval` **before** connect (`Units: AP beacon intervals. Defaults to 3 if 0` — see IDF types header). Example: beacon 100 TU ≈ 102.4 ms × listen_interval 3 ≈ **~300 ms** wake period ([power_save example Kconfig](https://github.com/espressif/esp-idf/blob/master/examples/wifi/power_save/main/Kconfig.projbuild)).

### Arduino-ESP32 3.x (not in official PIO 7.0.1)

Same `WiFi.setSleep(bool)` / `WiFi.setSleep(wifi_ps_type_t)` pattern ([3.3.4 WiFiGeneric.cpp](https://github.com/espressif/arduino-esp32/blob/3.3.4/libraries/WiFi/src/WiFiGeneric.cpp)). Prefer the enum overload when you need `MAX_MODEM` explicitly.

---

## 3. Automatic light sleep + `esp_pm_config` — does association stay up?

**Yes — if you use automatic Light-sleep via the power-management component together with modem sleep.** Manual `esp_light_sleep_start()` / deep sleep do **not** keep WiFi up.

| Path | Wi-Fi association | Docs |
|------|-------------------|------|
| Modem-sleep only (`esp_wifi_set_ps`) | **Maintain** | [Wi-Fi power-saving](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32c3/api-guides/wifi.html#esp32-c3-wi-fi-power-saving-mode) |
| Auto Light-sleep (`esp_pm_configure` + `light_sleep_enable`) + modem sleep | **Maintain** (driver wakes for DTIM / TX) | [Sleep modes — WiFi/BT](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32c3/api-reference/system/sleep_modes.html#wifi-bt-and-sleep-modes), [Low power Wi-Fi](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html) |
| Manual `esp_light_sleep_start()` | **Not maintained** (RF powered down; stop WiFi first) | Same sleep-modes page |
| Deep sleep | **Disconnect** | Same + summary table (“Disconnect”) |

IDF requirements ([Power management ESP32-C3 v4.4.7](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32c3/api-reference/system/power_management.html)):

1. Compile-time **`CONFIG_PM_ENABLE`**
2. For auto light sleep: **`CONFIG_FREERTOS_USE_TICKLESS_IDLE`**
3. Runtime:

```cpp
// IDF 4.4.7 Arduino package — chip-specific struct name
#include "esp_pm.h"

esp_pm_config_esp32c3_t pm = {
  .max_freq_mhz = 160,   // or 80
  .min_freq_mhz = 40,    // typically XTAL (often 40 on C3 modules)
  .light_sleep_enable = true,
};
esp_err_t err = esp_pm_configure(&pm);  // ESP_ERR_NOT_SUPPORTED if PM not in sdkconfig
```

On IDF 5.1+, the type is often `esp_pm_config_t` (same fields).

**Arduino practical caveat:** Stock Arduino-ESP32 **prebuilt** IDF libs frequently have `CONFIG_PM_ENABLE` / tickless idle **off**. Then `esp_pm_configure()` returns `ESP_ERR_NOT_SUPPORTED`. Enabling auto light sleep usually means Arduino-as-IDF-component or a custom lib build — not a one-liner in a normal PlatformIO Arduino env.

WiFi holds `ESP_PM_APB_FREQ_MAX` while started; with modem sleep, that lock is **released while the radio is gated**, so DFS/light sleep can actually run between DTIMs.

**Fit for this app:** Auto light sleep is Espressif’s path to ~1 mA average while remaining associated, but it conflicts with busy loops, USB-CDC, tight MQTT `loop()`, and bit-banged / timing-sensitive 433 MHz TX unless you take `ESP_PM_NO_LIGHT_SLEEP` around RF bursts. Treat as optional / later.

---

## 4. `setCpuFrequencyMhz(80)` with WiFi up

- ESP32-C3 max CPU is **160 MHz** ([datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf)); 80 MHz is a first-class PM frequency ([power management table](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32c3/api-reference/system/power_management.html)).
- Arduino: `setCpuFrequencyMhz(80)` / `getCpuFrequencyMhz()`; `board_build.f_cpu` only sets the **boot** default.
- WiFi can stay associated at 80 MHz; APB stays 80 MHz when CPU is 80 or 160 under the usual lock rules.
- Changing frequency at runtime has historically caused disconnects on some Arduino-ESP32 builds ([issue #7240](https://github.com/espressif/arduino-esp32/issues/7240)); safer pattern: change **after** stable IP, or set `board_build.f_cpu = 80000000L` so the core boots at 80 MHz. Re-init USB Serial after a runtime change if the monitor goes garbled.

Impact: lowers baseline CPU current (~3–6 mA class per datasheet modem-sleep rows) and heat; **does not** replace modem sleep for RF power.

---

## 5. DTIM / listen interval vs MQTT and HTTP

**DTIM** is set by the **router** (often 1 or 3). Beacon interval is commonly 100 TU (~102.4 ms).

| Setting | Typical downlink delay | MQTT / HTTP impact |
|---------|------------------------|--------------------|
| `PS_NONE` | ~immediate | Lowest latency; highest heat |
| `MIN_MODEM` | ≤ 1× DTIM (e.g. ~100–300 ms) | Fine for remote commands; AP buffers unicast until STA wakes |
| `MAX_MODEM` | ≤ listen_interval × beacon | Riskier: missed broadcasts; longer stalls; avoid unless profiling proves need |

**MQTT (PubSubClient):** Keepalive is typically **15 s**. That is far above DTIM latency. Requirements:

- Call `mqtt.loop()` often enough in the main loop (don’t block for seconds).
- Keep keepalive **well above** worst-case wake delay (always true for MIN_MODEM).
- Broker may drop the client if TCP stalls longer than keepalive — MIN_MODEM rarely causes that; deep sleep / long MAX listen intervals can.

**HTTP:** Client request is buffered by the AP while the STA sleeps; response starts after the next DTIM wake — usually sub-second. For a hood controller, that is acceptable.

**Do not** raise `listen_interval` aggressively for this app; prefer `MIN_MODEM` and let the AP’s DTIM decide.

---

## 6. What NOT to use

| Approach | Why not (for always-on MQTT/HTTP) | Source |
|----------|-----------------------------------|--------|
| **Deep sleep** | Wi-Fi **disconnects**; µA currents only after teardown | [Low power Wi-Fi table](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html), [Sleep modes](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32c3/api-reference/system/sleep_modes.html) |
| **Manual light sleep** (`esp_light_sleep_start`) without PM auto path | Wireless peripherals powered down; connection not maintained | Same sleep-modes page |
| **Modem sleep during bring-up** on this Super Mini | Contributed to **reason 34 (`DISASSOC_LOW_ACK`)** in project testing | [wifi-reliability notes](./esp32-c3-wifi-reliability.md) |
| **`WIFI_PS_MAX_MODEM` + large listen_interval** | Extra latency; missed DTIM/broadcast; little gain vs MIN for this duty cycle | IDF Wi-Fi power-save guide |
| **Assuming `WiFi.setSleep(true)` alone is enough for lowest power** | Only enables MIN modem sleep; auto light sleep / DFS need `CONFIG_PM_ENABLE` | PM docs + Arduino lib packaging |
| **High WiFi TX for “more reliability”** | On this board, high TX worsened auth / reason 34 | Project bring-up |

---

## 7. Recommended ladder for this firmware

Espressif’s own recommended modem config is **`WIFI_PS_MIN_MODEM` + DFS** (`light_sleep_enable = false`) in the [low-power Wi-Fi guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html). Adapted for **this** always-on controller and known RF fragility:

### Step A — Connect (stability first)

1. `WiFi.mode(WIFI_STA)`
2. **`WiFi.setSleep(false)` / `esp_wifi_set_ps(WIFI_PS_NONE)`**
3. Low TX (`WIFI_POWER_8_5dBm` ladder), optional 11b/g + BSSID (existing `connectSta`)
4. Wait until associated + **GOT_IP**

### Step B — After stable IP (power / heat)

1. Enable modem sleep: **`WiFi.setSleep(WIFI_PS_MIN_MODEM)`** or `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)`  
   (`setSleep(true)` is equivalent.)
2. Optional: **`setCpuFrequencyMhz(80)`** or `board_build.f_cpu = 80000000L` once WiFi is proven at 80 MHz.
3. Keep calling `mqtt.loop()` / `server.handleClient()`; no deep sleep.

### Step C — Safety net (implemented in `main.cpp`)

- On disconnect **reason 34** while modem sleep is active → force **`PS_NONE`** for the rest of the session (`gModemSleepAllowed = false`).
- Re-associate with PS_NONE; only re-try MIN_MODEM after a clean boot if desired.
- Also: `delay(1)` in `loop()` so FreeRTOS idle can run (busy-spin was a major heat source independent of modem sleep).

### Step D — Optional later (biggest mA drop)

- Only if heat/power still matters **and** you can rebuild with `CONFIG_PM_ENABLE` + tickless idle: `esp_pm_configure` with DFS (`light_sleep_enable` false first), then try auto light sleep.
- Around 433 MHz TX: acquire `ESP_PM_NO_LIGHT_SLEEP` (or temporarily disable light sleep) so bit timing stays stable.
- Skip for v1 if USB-CDC debugging and RF TX are primary.

### Explicitly skip for this product role

- Deep sleep / timed wake cycles that tear down STA
- `WIFI_PS_MAX_MODEM` unless measurements show MIN is insufficient **and** command latency is still OK

---

## Concrete API cheat sheet (Arduino 2.0.17 / IDF 4.4.7)

```cpp
#include <WiFi.h>
#include "esp_wifi.h"
// #include "esp_pm.h"  // only if CONFIG_PM_ENABLE in the linked IDF

// During connect:
WiFi.setSleep(false);                    // or WiFi.setSleep(WIFI_PS_NONE);
esp_wifi_set_ps(WIFI_PS_NONE);

// After GOT_IP:
WiFi.setSleep(WIFI_PS_MIN_MODEM);        // preferred overload
// WiFi.setSleep(true);                  // same as MIN_MODEM
// esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // identical effect

wifi_ps_type_t ps;
esp_wifi_get_ps(&ps);

// Optional heat cut:
setCpuFrequencyMhz(80);

// Do NOT for always-on MQTT/HTTP:
// esp_deep_sleep_start();
// esp_light_sleep_start();  // without auto-PM + modem sleep path
```

---

## Sources (primary)

1. [ESP32-C3 Wi-Fi Power-saving Mode — IDF v5.1](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32c3/api-guides/wifi.html#esp32-c3-wi-fi-power-saving-mode)  
2. [Same section — IDF v4.4.7](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32c3/api-guides/wifi.html) (matches Arduino 2.0.17)  
3. [Introduction to Low Power Mode in Wi-Fi Scenarios — ESP32-C3](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html)  
4. [Power Management — ESP32-C3 IDF v4.4.7](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32c3/api-reference/system/power_management.html)  
5. [Sleep Modes — ESP32-C3 IDF v4.4.7 (WiFi/BT and sleep)](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32c3/api-reference/system/sleep_modes.html#wifi-bt-and-sleep-modes)  
6. [ESP32-C3 Series Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf) (power modes + Table 5-8)  
7. [Arduino-ESP32 2.0.17 `WiFi.setSleep`](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/WiFi/src/WiFiGeneric.cpp)  
8. [PlatformIO espressif32 7.0.1 release notes](https://github.com/platformio/platform-espressif32/releases/tag/v7.0.1) (Arduino 2.0.17 / IDF 4.4.7)  
9. [IDF `wifi/power_save` example](https://github.com/espressif/esp-idf/blob/master/examples/wifi/power_save/main/power_save.c)
