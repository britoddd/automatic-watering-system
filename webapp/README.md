# Garden dashboard

Web dashboard for the ESP32 watering system at `https://garden.britod.my.id`.

```
ESP32 --TLS 8883--> Mosquitto <--localhost:1883-- server.js --SSE / HTTP--> nginx --> Cloudflare proxy --> browser
```

- `server.js`: Fastify app. Holds the only MQTT connection (user `webapp`), stores history in SQLite, pushes live state to browsers with Server-Sent Events, publishes commands after validating them, and handles login.
- `public/index.html`: the dashboard (vanilla JS, Chart.js from cdnjs). `public/login.html`: the login form. No build step.
- `deploy/`: systemd unit and nginx site config.

## Choices

| Topic | Choice | Why |
|---|---|---|
| Stack | Node + Fastify, plain HTML | No build step, about 50 MB RAM. Next.js builds can need 1 GB+, too much next to Factorio. |
| Login | One admin account in the app | No Cloudflare Zero Trust signup (it asks for a payment method). Password stored as a **scrypt** hash. Session = HMAC-signed cookie (`HttpOnly; Secure; SameSite=Strict`), 30 days. After 10 wrong passwords in 15 min, new logins are blocked for 15 min. Only Node's built-in `crypto`, no extra packages. |
| Live updates | SSE | One-way server-to-browser is all we need; commands go over normal `POST`. |
| Run as | systemd | Lighter than Docker, no image to build, and `localhost:1883` just works (Docker would need host networking). `DynamicUser` + `ProtectSystem=strict` sandbox it. |
| HTTPS | Cloudflare Origin Certificate, SSL mode **Full (strict)** | 15-year cert, no renewal job. The browser sees Cloudflare's cert; the origin cert only secures Cloudflare to VPS. |

## HTTP API

| Method | Path | Body | Notes |
|---|---|---|---|
| POST | `/login` | `{"username","password"}` | sets the session cookie; `401` wrong, `429` too many attempts |
| POST | `/logout` | | clears the cookie |
| GET | `/events` | | SSE stream: `{state, status, at, now, broker}` |
| GET | `/api/history?range=24h\|7d` | | `[{t, moisture, pump}]`; 1-min buckets for 24 h, 10-min for 7 d |
| POST | `/api/cmd/mode` | `{"value":"auto"\|"manual"}` | published **retained** |
| POST | `/api/cmd/threshold` | `{"value":5..60}` | published **retained** |
| POST | `/api/cmd/pump` | `{"value":"on"\|"off"}` | **never retained**. `on` refused unless online, manual, no fault, no lockout |

Everything except `/login` and `/logout` needs a valid session: `GET /` redirects to `/login`, everything else gets `401`. Bad values get `400`. Commands while the device is offline get `409`. Only `application/json` bodies are accepted (blocks cross-site form posts).

History: one row per minute (each state message updates the current minute; `pump` = 1 if the pump ran at any point in that minute). Rows older than 30 days are deleted hourly.

## Local development

Needs Node 20.6+.

```bash
cd webapp
npm install
npm test                      # self-test: input validation, password hash, session cookie
cp .env.example .env          # set MQTT_URL/MQTT_USER/MQTT_PASS, DB_PATH=garden.db
npm run hash-password         # paste the printed ADMIN_PASS_HASH=... line into .env, also set SESSION_SECRET
                              # (or AUTH_DISABLED=1 to skip login while developing)
npm start                     # http://127.0.0.1:3000
```

To use the VPS broker from your PC, tunnel it: `ssh -L 1883:localhost:1883 user@vps`.

## Deploy on the VPS

### Server layout

| What | Where |
|---|---|
| Repo clone (code runs from `webapp/`) | `/home/deploy/garden-web` |
| Deploy script (run by GitHub Actions) | `/home/deploy/deploy-garden.sh` (copy of `deploy/deploy-garden.sh`) |
| Secrets | `/etc/garden-web/garden-web.env` |
| SQLite DB (created by systemd) | `/var/lib/garden-web/garden.db` |
| Service | `garden-web` |

Pushing to `main` runs `.github/workflows/deploy.yml`, which SSHes in as `deploy` and runs the deploy script: pull, `npm ci`, `npm run build` (self-test), restart.

### 1. Node, the clone, the deploy script (as `deploy`)

```bash
# Node 22 LTS (NodeSource), as an admin user
curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash -
sudo apt install -y nodejs

# As deploy:
git clone <repo URL> ~/garden-web            # private repo: add a read-only deploy key first
cp ~/garden-web/webapp/deploy/deploy-garden.sh ~/deploy-garden.sh
chmod +x ~/deploy-garden.sh
cd ~/garden-web/webapp && npm ci
```

Let `deploy` restart the service without a password (`sudo visudo -f /etc/sudoers.d/garden-web`):

```text
deploy ALL=(root) NOPASSWD: /usr/bin/systemctl restart garden-web
```

### 2. Secrets

```bash
cd ~/garden-web/webapp
sudo mkdir -p /etc/garden-web
sudo cp .env.example /etc/garden-web/garden-web.env
node server.js --hash-password      # type the admin password, copy the printed line
openssl rand -hex 32                # copy for SESSION_SECRET
sudo nano /etc/garden-web/garden-web.env   # MQTT_PASS, ADMIN_USER, ADMIN_PASS_HASH, SESSION_SECRET; keep DB_PATH
sudo chmod 600 /etc/garden-web/garden-web.env
```

Do **not** set `AUTH_DISABLED` here. To change the password later: run `--hash-password` again, replace `ADMIN_PASS_HASH`, restart. To log out every device: change `SESSION_SECRET`, restart.

If Mosquitto uses an ACL file:

```text
user webapp
topic read garden/state
topic read garden/status
topic write garden/cmd/#

# the ESP32 also publishes cmd/mode and cmd/threshold (offline control page)
user esp32
topic write garden/state
topic write garden/status
topic readwrite garden/cmd/#
```

### 3. Cloudflare

1. **DNS**: `A garden -> <VPS IP>`, proxy **on** (orange cloud).
2. **Origin certificate**: SSL/TLS > Origin Server > Create Certificate (hostname `garden.britod.my.id`, PEM). Save on the VPS:

   ```bash
   sudo mkdir -p /etc/ssl/cloudflare
   sudo nano /etc/ssl/cloudflare/garden.pem   # certificate
   sudo nano /etc/ssl/cloudflare/garden.key   # private key
   sudo chmod 600 /etc/ssl/cloudflare/garden.key
   ```

3. **SSL mode**: SSL/TLS > Overview > **Full (strict)**. If other hostnames on the zone need a different mode, set it only for `garden.britod.my.id` with a Configuration Rule.

All of this is on the free Cloudflare plan. Zero Trust is not used.

The `mqtt` record must stay **DNS only** (grey cloud); Cloudflare's proxy does not carry MQTT.

### 4. systemd and nginx

```bash
cd ~/garden-web/webapp
sudo cp deploy/garden-web.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now garden-web
systemctl status garden-web        # should say "garden dashboard on http://127.0.0.1:3000"
curl -sI http://127.0.0.1:3000/    # 302 -> /login

sudo apt install -y nginx          # if not installed yet
sudo cp deploy/garden.nginx.conf /etc/nginx/sites-available/garden
sudo ln -s /etc/nginx/sites-available/garden /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

### 5. Firewall (Tencent Lighthouse console > Firewall)

- TCP **80, 443**: open.
- TCP **8883**: open (ESP32, already there).
- TCP **1883** and **3000**: keep **closed**. Both only listen on localhost anyway.

Logs: `journalctl -u garden-web -f`. Update: push to `main` (or run `~/deploy-garden.sh` by hand). If you change `garden-web.service`, copy it to `/etc/systemd/system/` again and run `sudo systemctl daemon-reload`; the deploy script doesn't do that.

## Test plan

Run the fakes on the VPS **with the ESP32 unplugged**, or the real device will overwrite the fake state.

```bash
ESP="-h localhost -u esp32 -P YOUR_ESP32_PASSWORD"
state() {  # state <moisture> <pump> <auto> <fault> <locked> <bursts>
  mosquitto_pub $ESP -r -t garden/state -m "{\"moisture\":$1,\"raw\":1800,\"pump\":$2,\"auto\":$3,\"fault\":$4,\"bursts\":$6,\"threshold\":15,\"locked\":$5,\"uptime\":$SECONDS}"
}
mosquitto_pub $ESP -r -t garden/status -m online

# Watch what the app publishes, with the retain flag:
mosquitto_sub -h localhost -u webapp -P YOUR_WEBAPP_PASSWORD -t 'garden/cmd/#' -F '%t %p retain=%r'
```

| # | Do | Expect |
|---|---|---|
| 1 | `while true; do state 42 0 1 0 0 0; sleep 5; done` | Badge **Online**, 42 %, mode Auto, "Last update" counts 0-5 s. "Water now" disabled ("Switch to Manual mode"). |
| 2 | Click **Manual** | sub shows `garden/cmd/mode manual retain=1`. (Mode only changes on screen once the device echoes it; with fakes, send `state 42 0 0 0 0 0`.) |
| 3 | Loop with `state 42 0 0 0 0 0`, click **Water now** | sub shows `garden/cmd/pump on retain=0`. |
| 4 | `state 42 1 0 0 0 0` | Pump **ON**, button turns into **Stop pump**; click it: `garden/cmd/pump off retain=0`. |
| 5 | Drag threshold to 30 and release | `garden/cmd/threshold 30 retain=1`. |
| 6 | `state 42 0 0 1 0 0` | Red **Sensor fault** banner, Water now disabled. |
| 7 | `state 10 0 1 0 1 5` | Red **Safety lockout** banner, Water now disabled. |
| 8 | Stop the loop, wait 30 s | Badge **Offline**, yellow banner, all controls disabled. |
| 9 | `mosquitto_pub $ESP -r -t garden/status -m offline` | Offline immediately, even if state is fresh. |
| 10 | `sudo systemctl restart mosquitto` | Banner shows briefly; app reconnects by itself. |
| 11 | Run the loop for a few minutes, reload | History chart shows the points. Toggle 24 h / 7 d. |
| 12 | Phone browser | Cards stack in one column, buttons easy to tap. |
| 13 | Private window to `https://garden.britod.my.id` | Redirects to the login page. Wrong password: "Wrong username or password". Right password: dashboard. |
| 14 | `curl -s https://garden.britod.my.id/api/history` | `401 login required` (API is closed without a session). |
| 14b | Click **Log out**, then press Back | Back on the login page; dashboard data does not load. |
| 14c | 10 wrong passwords in a row | `Too many attempts. Try again in 15 minutes.` A device that's already logged in keeps working. |
| 15 | `cd ~/garden-web/webapp && npm test` | `selftest ok` (input validation, password hash, session cookie). |

Input validation against the live server (local dev with `AUTH_DISABLED=1`):

```bash
curl -s -H 'content-type: application/json' -d '{"value":99}'   localhost:3000/api/cmd/threshold   # 400
curl -s -H 'content-type: application/json' -d '{"value":"ON"}' localhost:3000/api/cmd/pump        # 400
curl -s -H 'content-type: text/plain'       -d '{"value":"on"}' localhost:3000/api/cmd/pump        # 415
```

Clean up the fakes afterwards (the ESP32 republishes its real state when it reconnects):

```bash
mosquitto_pub $ESP -r -t garden/state -n
mosquitto_pub $ESP -r -t garden/status -m offline
```
