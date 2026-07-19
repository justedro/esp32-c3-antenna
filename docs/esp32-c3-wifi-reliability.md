# ESP32-C3 Super Mini — WiFi reliability notes

Findings from bringing up STA WiFi for this project (Novy hood controller on STX882 / GPIO4).

## Hardware

| Item | Notes |
|------|--------|
| Board | ESP32-C3 Super Mini (USB Serial/JTAG, VID `303A:1001`) |
| Band | **2.4 GHz only** (no 5 GHz). Channels 1/6/11 are normal. |
| Antenna | Onboard ceramic; RF path is sensitive to TX power |

One module never showed SoftAP / failed STA with `AUTH_EXPIRE` despite strong scans (RX OK, TX weak/faulty). A second module worked once TX power was reduced.

## Symptom: disconnect reason 34

**Code 34 = `WIFI_REASON_DISASSOC_LOW_ACK` (IEEE “missing ACKs” / poor channel conditions).**

The AP associates (or starts to), then kicks the station because it is not getting reliable acknowledgements. On ESP32-C3 Super Mini this often looks like:

- Scan sees the SSID at strong RSSI (e.g. **−48…−50 dBm**)
- Router may show few/no successful clients
- SoftAP may work at short range while home STA fails (or vice versa)
- Raising TX to maximum can make things **worse**

RSSI is **downlink** (what the ESP hears from the AP). It stays good even when STA TX is the problem.

## What fixed STA for us

1. **Platform upgrade** — PlatformIO `espressif32@7.0.1` (newer Arduino-ESP32 / IDF WiFi stack) instead of 6.13.0.
2. **Lower TX power first** — start at `WIFI_POWER_8_5dBm` (not 19.5 dBm). This was the decisive fix; first successful connect used low power with RSSI ≈ **−49**.
3. **`esp_wifi_set_ps(WIFI_PS_NONE)`** — disable modem sleep (sleep can contribute to missing ACKs).
4. **Prefer 802.11b/g** — `WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G` before enabling 11n.
5. **Connect by BSSID + channel** to the strongest matching AP after a scan.
6. SoftAP fallback if STA fails (`novy-hood` / `novyhood1` → `http://192.168.4.1`).

Working result example:

```text
Best AP RSSI=-48 ch=6
TX power enum=34 bgOnly=yes BSSID ch=6   # WIFI_POWER_8_5dBm
[WiFi] associated
[WiFi] got IP <dhcp-ip>
STA OK  IP=<dhcp-ip>  RSSI=-49  TX=60
```

## What did *not* help (or misled)

| Approach | Outcome |
|----------|---------|
| Max TX (`WIFI_POWER_19_5dBm` / high `esp_wifi_set_max_tx_power`) | Auth expire / reason 34 on C3 Super Mini |
| Assuming “strong RSSI ⇒ TX is fine” | Wrong — RSSI ≠ uplink quality |
| Assuming wrong password only | SoftAP + low-power STA proved radio could work |
| Endless protocol/PMF toggles alone | Insufficient without lowering TX power |

## Practical checklist

1. Confirm SSID is 2.4 GHz and visible in a scan.
2. Use a recent `espressif32` platform (7.x+ recommended here).
3. After `WiFi.mode(WIFI_STA)` / `begin`, set **low TX**, **PS_NONE**, optionally **11b/g** + **BSSID**.
4. If still failing, try SoftAP at close range to prove TX beacons.
5. If SoftAP is invisible even against the phone and scans still work → suspect bad board/antenna (RX-only).
6. Power: short USB cable / solid 5 V supply; RF TX spikes can brown out flaky supplies.

## Power saving (WiFi stays associated)

See [esp32-c3-power-saving.md](./esp32-c3-power-saving.md) for Espressif sources and the full ladder. Firmware: connect with `PS_NONE`, then `MIN_MODEM` + 80 MHz CPU + `delay(1)` in `loop()`; fall back to `PS_NONE` on reason 34. Check serial `[PWR]…` or `GET /api/status` (`wifi_ps` / `cpu_mhz`).

## Code location

STA connect ladder + SoftAP fallback live in `src/main.cpp` (`connectSta` / `startSoftAp`). WiFi credentials and Novy channel: `include/secrets.h` (gitignored; see `secrets.example.h`).

## Breadboard kills WiFi (confirmed)

An **empty breadboard** alone was enough to break STA (reason 2 / flaky assoc) on this Super Mini.

**Why:** solderless breadboards add large parasitic C/L, poor RF ground, and couple noise into the antenna / nearby pins. 2.4 GHz WiFi is especially sensitive; the board’s tiny ceramic antenna “sees” the breadboard as a detuned mess even with no other parts wired.

**What to do:**

- Develop WiFi **off** the breadboard (Dupont/direct solder / proto PCB).
- If you must use a breadboard: only for low-speed I/O; keep ESP **antenna end hanging off** the board; shortest possible jumpers; never run long VCC/GND rails under the antenna.
- 433 MHz TX can stay on a breadboard more easily than the ESP itself — prefer the ESP freestanding and only DATA/GND/5V flying wires to the STX882.

This also explains “works on desk, dies when I wire peripherals”: often the breadboard, not the STX882 current alone.

When STX882 is powered from the ESP **3.3 V** rail, WiFi often fails again with **reason 2 (`AUTH_EXPIRE`)** even with low WiFi TX power. Scan/RSSI can still look fine.

**Cause:** STX882 idle + transmit current (and RF noise) stresses the Super Mini 3.3 V LDO shared with WiFi. A 47 µF on the module helps but is often not enough if VCC is 3.3 V.

### Rewire (recommended)

| STX882 | Connect to |
|--------|------------|
| **VCC** | ESP **5V** (USB 5 V pin), **not** 3V3 |
| **GND** | ESP GND (common ground required) |
| **DATA** | GPIO4 |
| Caps | **100 nF ceramic ∥ 100–470 µF** electrolytic right at STX882 VCC–GND |
| Optional | Extra **100 µF+** on ESP **3V3–GND** near the board |

Also:

- Keep 433 antenna and WiFi antenna / USB cable physically apart (few cm).
- Prefer short thick wires; avoid long breadboard rails for VCC/GND.
- Firmware connects WiFi **before** enabling RF DATA; DATA is driven only while sending.

### Confirmed working setup

- ESP32-C3 Super Mini **off** breadboard (or antenna clear of it)
- STX882 on **5V** + GND + DATA→GPIO4
- **~220 µF** bulk cap at the TX (plus ceramic if available)
- WiFi: platform `espressif32@7.0.1`, low TX power, connect with `PS_NONE` then `MIN_MODEM` + 80 MHz CPU
- Control over STA WiFi; Flipper sees steady Novy/RCSwitch bursts when commands are sent from the web UI
- No WiFi dropouts observed in this configuration
