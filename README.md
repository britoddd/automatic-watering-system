# Automatic Plant Watering System (ESP32 + MQTT)

An ESP32 reads a soil-moisture sensor and waters a plant through a relay-driven pump. It runs on its own, with or without internet. A Mosquitto broker on a VPS carries the data over MQTT/TLS to a small web dashboard at `https://garden.britod.my.id`, where you can watch the plant live, see its history and control the pump. No paid IoT platform is used.

```mermaid
flowchart LR
  S[Soil sensor] --> E[ESP32]
  E --> R[Relay] --> P[Pump]
  Phone1[Phone on ESP32 hotspot] -- "http://192.168.4.1" --> E
  E -- "MQTT over TLS :8883" --> M[(Mosquitto on VPS)]
  M -- "localhost :1883" --> W[Web app: Node + Fastify + SQLite]
  W -- "nginx :443" --> CF[Cloudflare proxy] --> B[Browser]
```

## Features

- **Automatic watering**: 3 s bursts when moisture drops below the threshold, 5 min soak time between bursts.
- **Safety**:
  - A manual run stops by itself after 10 s.
  - After 5 bursts in a row with the soil still dry, the ESP32 locks out the pump until it is physically reset.
  - An invalid sensor reading blocks the pump.
- **Offline control**: the ESP32 always runs its own Wi-Fi hotspot with a local control page. It works with no router and no internet.
- **Wi-Fi setup without reflashing**: change the router (or phone hotspot) the ESP32 uses from the hotspot page. The network is saved in flash.
- **Web dashboard**:
  - Live state over Server-Sent Events.
  - Mode switch, threshold slider and "Water now" button.
  - 24 h / 7 d history chart.
  - Login page.
  - shadcn-style design with light and dark themes.
- **Continuous deployment**: every push to `main` updates the VPS through GitHub Actions.

## Repository layout

| Path | What |
|---|---|
| `watering_system_mqtt.ino` | ESP32 firmware (watering logic, MQTT client, hotspot page, Wi-Fi setup) |
| `secrets.example.h` | Template for `secrets.h` (Wi-Fi, hotspot, MQTT credentials, root CAs) |
| `webapp/` | Dashboard backend and frontend. Setup, deploy and test plan: [webapp/README.md](webapp/README.md) |
| `webapp/deploy/` | systemd unit, nginx site, deploy script |
| `.github/workflows/deploy.yml` | CD: SSH into the VPS and run the deploy script |
| `watering_web_app_brief.md` | Original requirements for the web app |

## Hardware

| Part | Connection |
|---|---|
| Soil moisture sensor VCC / GND | ESP32 VIN (5 V) / GND (check your sensor's voltage) |
| Soil moisture sensor AOUT | GPIO 34 (ADC1, works while Wi-Fi is on) |
| Relay module IN | GPIO 26 (active-LOW by default: `RELAY_ACTIVE_LOW`) |
| Relay module VCC / GND | Supply suitable for the relay module |
| Pump | **Its own power supply**, switched through the relay contacts |
| Water level strip "+" / "−" / "S" (optional) | GPIO 25 (powers it only while reading) / GND / GPIO 35 (ADC1). See [Water tank level](#water-tank-level-optional) |

Notes:
- **Flyback diode:** put one (1N4007 or 1N5819) across the pump terminals, stripe to +.
- **Separate pump power:** don't power the pump from the ESP32 or USB. Motor surges on a shared 5 V line can make a PC cut power to its USB port.
- **Calibration:** set `DRY_VALUE` and `WET_VALUE` in the sketch for your sensor. Set `CALIBRATION_MODE = true` to read raw values without the pump running automatically.
- **First test:** run it with the pump disconnected, to confirm the relay polarity.

## MQTT contract

| Topic | Direction | Payload | Retained |
|---|---|---|---|
| `garden/state` | ESP32 → | `{"moisture":0-100,"raw":int,"pump":0/1,"auto":0/1,"fault":0/1,"bursts":int,"threshold":5-60,"locked":0/1,"uptime":s}`, every 5 s and on change | yes |
| `garden/status` | ESP32 → | `online` / `offline` (`offline` is the Last Will) | yes |
| `garden/cmd/mode` | → ESP32 | `auto` / `manual` | yes |
| `garden/cmd/threshold` | → ESP32 | `5` to `60` | yes |
| `garden/cmd/pump` | → ESP32 | `on` / `off` (manual mode only) | **never** (a retained `on` would start the pump on every reconnect) |

When mode or threshold is changed on the hotspot page, the ESP32 itself publishes the new value (retained) to the `cmd` topic, so the broker never undoes a local change.

With the water level sensor enabled, `garden/state` also carries `"water":0-100,"waterRaw":int,"tankLow":0/1`. Without it, those fields are absent, and both dashboards hide everything tank-related.

## Water tank level (optional)

All the code for an **analog water level strip** is already in place and switched off. To add the sensor:

1. **Wire it**: strip `+` → GPIO 25, `−` → GND, `S` → GPIO 35.
   - The firmware powers the strip from GPIO 25 only for the ~30 ms of each reading. A strip with constant current through the water corrodes within weeks.
   - The 3.3 V from the GPIO also keeps the signal inside the ESP32's ADC range.
2. **Mount it** so its bottom end is at the lowest water level the pump may still draw from. The strip only measures its own length, so "0 %" means "the water no longer reaches the strip".
3. **Turn it on**: in `watering_system_mqtt.ino` set `WATER_LEVEL_ENABLED = true`.
4. **Calibrate**:
   1. Set `CALIBRATION_MODE = true` and upload.
   2. In the Serial Monitor, note `Water tank raw` with the strip out of the water: that's `WATER_EMPTY_RAW`.
   3. Note it again with the strip submerged up to its top mark: that's `WATER_FULL_RAW`.
   4. Enter both values, set `CALIBRATION_MODE = false`, and upload again.
5. **Optional**: adjust `WATER_LOW_PERCENT` (default 15). The pump is blocked at or below it, and released again above `WATER_OK_PERCENT` (default 25).

What it does once enabled:
- **Pump protection:** below the low level, no pump run of any kind starts (automatic, manual, local page, web), and a running pump stops. This protects the pump from running dry.
- **No false lockout:** an empty tank doesn't count toward the "5 bursts with dry soil" safety lockout. Automatic watering resumes by itself after a refill.
- **Fail-safe:** a disconnected strip reads about 0, so it counts as "low" and the pump stays blocked.
- **Dashboards:**
  - The online dashboard shows a **Water tank** card, a red "Water tank low" alert, and a dashed tank line in the history chart. The level is stored with the history (the database column is added automatically).
  - The hotspot page shows a "Water tank" row and a low-tank warning.

## Getting started

### 1. Firmware
1. Install the Arduino IDE with the **ESP32 core** and the **PubSubClient** library (Nick O'Leary).
2. Copy `secrets.example.h` to `secrets.h` and fill it in. Use long random passwords for `MQTT_PASS` and `AP_PASSWORD`: port 8883 is reachable from the internet.
3. Open `watering_system_mqtt.ino`, select your ESP32 board and upload.
4. Open the Serial Monitor at 115200 baud. You should see the hotspot start, then `Connecting to MQTT broker ... connected.`

### 2. Broker (Mosquitto on the VPS)
Two listeners: plain MQTT on localhost for the web app, and TLS for the ESP32. Example `/etc/mosquitto/conf.d/garden.conf`, to adapt to your setup:

```conf
listener 1883 127.0.0.1

listener 8883
certfile /etc/mosquitto/certs/fullchain.pem   # Let's Encrypt cert for mqtt.<domain>, readable by mosquitto
keyfile  /etc/mosquitto/certs/privkey.pem

allow_anonymous false
password_file /etc/mosquitto/passwd
acl_file /etc/mosquitto/acl
```

Users (`sudo mosquitto_passwd /etc/mosquitto/passwd <user>`; add `-c` only when creating the file for the very first time, because it wipes existing users):
- `esp32`: same password as `MQTT_PASS` in `secrets.h`
- `webapp`: same password as `MQTT_PASS` in the web app's env file

ACL (`/etc/mosquitto/acl`):

```text
user esp32
topic readwrite garden/#

user webapp
topic read garden/state
topic read garden/status
topic readwrite garden/cmd/#
```

DNS: the `mqtt` record must be **DNS only** (grey cloud in Cloudflare). Cloudflare's proxy doesn't carry MQTT.

### 3. Web app
Node 20.6+ (Node 22 LTS recommended), systemd, nginx and a Cloudflare origin certificate. Every step is in [webapp/README.md](webapp/README.md).

### 4. Continuous deployment
The workflow SSHes into the VPS as `deploy` and runs `~/deploy-garden.sh`: pull `main`, `npm ci`, self-test, restart `garden-web`. Repository secrets:

| Secret | Value |
|---|---|
| `VPS_HOST` | VPS IP, or a DNS-only hostname (not a Cloudflare-proxied one) |
| `VPS_SSH_KEY` | Private key (`-----BEGIN OPENSSH PRIVATE KEY-----` ... END). Its `.pub` goes in `/home/deploy/.ssh/authorized_keys` |
| `VPS_KNOWN_HOSTS` | Output of `ssh-keyscan -t ed25519 <VPS_HOST>` (the full key line, not the fingerprint) |

## Using it

- **Online:** open `https://garden.britod.my.id` and log in. The badge shows **Online** while the ESP32 reports every 5 s. Use the sun/moon button for light or dark mode.
- **Offline:** connect a phone to the ESP32's hotspot (`AP_SSID`), open `http://192.168.4.1` and log in with `LOCAL_USER` / `LOCAL_PASS`.
- **New Wi-Fi:** on the hotspot page, enter the network under **Router Wi-Fi** and tap **Save and connect**. The phone may drop off the hotspot for a few seconds while the ESP32 joins the new network.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Serial: `failed, state = -2` | TLS failed: `ROOT_CA` is wrong, or its BEGIN/END lines are missing | Use the root certs from `secrets.example.h`. Check port 8883 is open and the `mqtt` DNS record is grey-cloud |
| Serial: `state = 4` or `5`; web app log: `Not authorized` | Password doesn't match Mosquitto's | Reset it with `mosquitto_passwd` (no `-c`) to the value in `secrets.h` / env file |
| Dashboard "Device offline", web app SSE shows `"state":null` | `garden/state` is never delivered | Check the ACL (above); test with `mosquitto_sub -u webapp ... -t garden/state` |
| "Broker unreachable" banner | The web app can't log in to Mosquitto | `journalctl -u garden-web`; check `MQTT_*` in `/etc/garden-web/garden-web.env` (no CR or trailing spaces) |
| `garden-web` crashes with a segmentation fault | Node 18 (Ubuntu's apt version) with a native module built for newer Node | Install Node 22 from NodeSource, then `rm -rf node_modules && npm ci` |
| Action: `Host key verification failed` | `VPS_KNOWN_HOSTS` missing, or made for a different host string than `VPS_HOST` | Re-run `ssh-keyscan` with exactly the `VPS_HOST` value |
| Action: `error in libcrypto` / `Permission denied (publickey)` | Bad `VPS_SSH_KEY`, or its `.pub` isn't in `authorized_keys` | Re-paste the whole private key (`cat key \| clip`); add the `.pub` for `deploy` |
| PC shuts down while the pump switches | Pump surges on the USB 5 V / ground | Separate pump supply, flyback diode, powered USB hub |

## Security notes

- **Secrets stay out of git:** they live in `secrets.h` (ESP32) and `/etc/garden-web/garden-web.env` (VPS), and both are in `.gitignore`.
- **Dashboard login:** scrypt-hashed password, signed `HttpOnly; Secure; SameSite=Strict` session cookie, and a lock on new logins after 10 failures in 15 minutes.
- **MQTT stays on the server:** the browser never talks to MQTT or sees MQTT credentials. The server checks every command before publishing it.
- **Open ports:** only 80/443 (web) and 8883 (MQTT over TLS). Port 1883 and the app's port 3000 listen on localhost only.
