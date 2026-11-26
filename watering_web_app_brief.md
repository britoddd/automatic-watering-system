# Brief: Web dashboard for ESP32 automatic watering system

## Goal
Build a small web app that shows live data from my ESP32 watering system and lets me control it. It runs on my Tencent Cloud VPS and is reachable at `https://garden.britod.my.id`. No paid IoT platforms (no Blynk, no cloud dashboards with credit limits).

## Existing setup (already working, do not reinstall)
- VPS: Tencent Cloud Lighthouse, Ubuntu, 2 vCPU, 2 GB RAM, 40 GB disk. A Factorio server also runs on it in Docker, so keep the app light on memory.
- MQTT broker: Mosquitto on the same VPS.
  - `localhost:1883`: plain MQTT, localhost only. **The app must use this.**
  - `mqtt.britod.my.id:8883`: TLS (Let's Encrypt), used by the ESP32 over the internet.
  - Auth: username/password. Users: `esp32` (device) and `webapp` (this app). Anonymous access is off.
- Domain DNS is on Cloudflare (`britod.my.id`).
- The ESP32 firmware is finished. Do not change the MQTT contract below.

## MQTT contract (fixed)
ESP32 publishes:
| Topic | Payload | Notes |
|---|---|---|
| `garden/state` | JSON: `{"moisture":0-100,"raw":int,"pump":0/1,"auto":0/1,"fault":0/1,"bursts":int,"threshold":5-60,"locked":0/1,"uptime":seconds}` | Retained. Sent every 5 s and on each change. |
| `garden/status` | `online` / `offline` | Retained. `offline` is the Last Will. |

App publishes (commands):
| Topic | Payload | Retain |
|---|---|---|
| `garden/cmd/mode` | `auto` or `manual` | **Yes** (ESP32 restores it on reconnect) |
| `garden/cmd/threshold` | integer string `5`–`60` | **Yes** |
| `garden/cmd/pump` | `on` or `off` | **NEVER** (a retained `on` would start the pump on every reconnect) |

The ESP32 also has an offline control page (its own Wi-Fi hotspot). When mode or threshold is changed there, the ESP32 itself publishes the new value retained to `garden/cmd/mode` / `garden/cmd/threshold`. So the app is not the only publisher on those topics, and pump/mode/threshold can change without an app command. Always show the real values from `garden/state`.

Device rules to reflect in the UI:
- A manual pump command only works in `manual` mode. The ESP32 stops a manual run after 10 s by itself.
- `locked: 1` means a safety lockout (soil still dry after 5 bursts). Only a physical ESP32 reset clears it. Show a clear warning.
- `fault: 1` means the sensor reading is invalid. The pump is blocked.
- If `garden/status` is `offline`, or `garden/state` is older than about 30 s, show the device as offline and disable the controls.

## Features
1. **Live dashboard:** moisture %, raw ADC value, pump on/off, mode, threshold, fault, lockout, burst count, online/offline, time of the last update. Updates live without a page reload.
2. **Controls:** mode switch (auto/manual), manual "Water now" button (enabled only in manual mode, online, no fault, no lockout), threshold slider or input (5–60).
3. **History:** store `garden/state` in SQLite (one row per minute is enough). Chart of moisture and pump events for the last 24 h / 7 d. Delete data older than 30 days.
4. Mobile-friendly layout.

## Architecture requirements
- The backend holds the only MQTT connection (`localhost:1883`, user `webapp`). The browser never connects to MQTT and never receives MQTT credentials.
- Push live updates to the browser with Server-Sent Events or WebSocket.
- Commands go through backend HTTP endpoints. Validate all input on the server (mode in {auto, manual}, pump in {on, off}, threshold an integer 5–60).
- Secrets only in `.env` (provide `.env.example`). Never commit `.env`.
- Suggested stack: Node.js + TypeScript, Next.js or a small Express/Fastify server, `mqtt` npm package, SQLite (`better-sqlite3`). Propose something lighter if Next.js builds are too heavy for 2 GB RAM.

## Security
- The site must require login. Preferred: Cloudflare Access in front of `garden.britod.my.id` (Cloudflare proxy on). If you choose app-level login instead, use one admin account with a hashed password and secure session cookies.
- Open only ports 80/443 for the web app. Do not open 1883.

## Deployment
- Run as a systemd service (or Docker Compose, your choice; explain why).
- nginx reverse proxy for `garden.britod.my.id`, with SSE/WebSocket support (no response buffering for the live stream).
- HTTPS: Cloudflare proxied record (orange cloud) with an origin certificate or Let's Encrypt. Explain the option you choose.
- Remember to open the port in the Tencent Lighthouse firewall if one is needed.

## Deliverables
- Source code, `README.md` with setup and deploy steps, `.env.example`, systemd unit (or compose file), nginx site config.
- A short test plan. Include `mosquitto_pub` commands that fake `garden/state` messages, so the UI can be tested without the ESP32.

## Before you start
Ask me about any unclear point. Confirm the stack and the login method with me before you write code.
