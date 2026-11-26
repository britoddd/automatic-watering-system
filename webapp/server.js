// Garden dashboard backend: one MQTT connection, SQLite history, SSE to browsers.
import { readFileSync } from 'node:fs';
import { scryptSync, randomBytes, timingSafeEqual, createHmac } from 'node:crypto';
import assert from 'node:assert/strict';

const env = process.env;
const STALE_MS = 30_000;
const SESSION_DAYS = 30;

// ---------- Command validation (server is the trust boundary) ----------
const COMMANDS = {
  mode:      { retain: true,  parse: v => (v === 'auto' || v === 'manual') ? v : null },
  threshold: { retain: true,  parse: v => /^\d{1,2}$/.test(String(v)) && +v >= 5 && +v <= 60 ? String(+v) : null },
  pump:      { retain: false, parse: v => (v === 'on' || v === 'off') ? v : null }, // NEVER retain pump
};

// ---------- Login: scrypt password hash + HMAC-signed session cookie ----------
// Hash format "scrypt:<salt hex>:<hash hex>" (no "$", so .env and systemd read it literally).
function hashPassword(pw) {
  const salt = randomBytes(16);
  return `scrypt:${salt.toString('hex')}:${scryptSync(pw, salt, 64).toString('hex')}`;
}
function checkPassword(pw, stored) {
  const [alg, salt, hash] = String(stored).split(':');
  if (alg !== 'scrypt' || !salt || !hash) return false;
  const want = Buffer.from(hash, 'hex');
  const got = scryptSync(String(pw ?? ''), Buffer.from(salt, 'hex'), want.length);
  return want.length > 0 && timingSafeEqual(got, want);
}
const sign = (data, secret) => createHmac('sha256', secret).update(data).digest('base64url');
// ponytail: stateless session (expiry + HMAC). No per-session logout; change SESSION_SECRET to log out every device.
const makeSession = (secret, days = SESSION_DAYS) => { const exp = String(Date.now() + days * 86_400_000); return `${exp}.${sign(exp, secret)}`; };
function validSession(token, secret) {
  const [exp, mac] = String(token || '').split('.');
  if (!exp || !mac) return false;
  const want = Buffer.from(sign(exp, secret)), got = Buffer.from(mac);
  return got.length === want.length && timingSafeEqual(got, want) && Number(exp) > Date.now();
}

if (process.argv[2] === '--selftest') {
  const p = (c, v) => COMMANDS[c].parse(v);
  assert.equal(p('mode', 'auto'), 'auto');
  assert.equal(p('mode', 'AUTO'), null);
  assert.equal(p('pump', 'on'), 'on');
  assert.equal(p('pump', true), null);
  assert.equal(p('threshold', 5), '5');
  assert.equal(p('threshold', '60'), '60');
  assert.equal(p('threshold', 4), null);
  assert.equal(p('threshold', 61), null);
  assert.equal(p('threshold', '12.5'), null);
  assert.equal(p('threshold', '1e1'), null);
  assert.equal(p('threshold', -10), null);
  assert.equal(COMMANDS.pump.retain, false);

  const h = hashPassword('correct horse');
  assert.equal(checkPassword('correct horse', h), true);
  assert.equal(checkPassword('wrong', h), false);
  assert.equal(checkPassword(undefined, h), false);
  assert.equal(checkPassword('x', 'garbage'), false);
  const s = makeSession('k'.repeat(32));
  assert.equal(validSession(s, 'k'.repeat(32)), true);
  assert.equal(validSession(s, 'z'.repeat(32)), false);                 // other secret
  assert.equal(validSession(s.replace(/^\d/, '9'), 'k'.repeat(32)), false); // tampered expiry
  assert.equal(validSession(makeSession('k'.repeat(32), -1), 'k'.repeat(32)), false); // expired
  assert.equal(validSession('', 'k'.repeat(32)), false);
  console.log('selftest ok');
  process.exit(0);
}

if (process.argv[2] === '--hash-password') {
  const { createInterface } = await import('node:readline/promises');
  const rl = createInterface({ input: process.stdin, output: process.stdout });
  const pw = await rl.question('New admin password (min 10 chars): ');
  rl.close();
  if (pw.length < 10) { console.error('Too short.'); process.exit(1); }
  console.log(`ADMIN_PASS_HASH=${hashPassword(pw)}`);
  process.exit(0);
}

const { default: Fastify } = await import('fastify');
const { default: mqtt } = await import('mqtt');
const { default: Database } = await import('better-sqlite3');

const AUTH_DISABLED = env.AUTH_DISABLED === '1';
if (AUTH_DISABLED) console.warn('WARNING: AUTH_DISABLED=1, no login check. Local testing only.');
else if (!env.ADMIN_USER || !env.ADMIN_PASS_HASH?.startsWith('scrypt:') || (env.SESSION_SECRET || '').length < 32)
  throw new Error('Set ADMIN_USER, ADMIN_PASS_HASH (npm run hash-password) and SESSION_SECRET (32+ chars), or AUTH_DISABLED=1 for local tests');

// ---------- SQLite history: one row per minute ----------
const db = new Database(env.DB_PATH || 'garden.db');
db.pragma('journal_mode = WAL');
db.exec(`CREATE TABLE IF NOT EXISTS history (
  ts INTEGER PRIMARY KEY,  -- unix seconds, start of minute
  moisture INTEGER, raw INTEGER,
  pump INTEGER             -- 1 if the pump was on at any point in this minute
)`);
// Water tank level (%), NULL while no level sensor is fitted. Added to existing databases once.
if (!db.prepare("SELECT 1 FROM pragma_table_info('history') WHERE name = 'water'").get()) {
  db.exec('ALTER TABLE history ADD COLUMN water INTEGER');
}
// Upsert every state message into its minute bucket; max() keeps short pump bursts visible.
const upsert = db.prepare(`INSERT INTO history (ts, moisture, raw, pump, water) VALUES (?, ?, ?, ?, ?)
  ON CONFLICT(ts) DO UPDATE SET moisture = excluded.moisture, raw = excluded.raw,
    pump = max(pump, excluded.pump), water = excluded.water`);
const prune = db.prepare('DELETE FROM history WHERE ts < ?');
const historyQuery = db.prepare(`SELECT (ts / @bucket) * @bucket AS t, round(avg(moisture)) AS moisture,
  max(pump) AS pump, round(avg(water)) AS water
  FROM history WHERE ts >= @since GROUP BY t ORDER BY t`);
const RANGES = { '24h': { secs: 86_400, bucket: 60 }, '7d': { secs: 604_800, bucket: 600 } };

const pruneOld = () => prune.run(Math.floor(Date.now() / 1000) - 30 * 86_400);
pruneOld();
setInterval(pruneOld, 3_600_000).unref();

// ---------- MQTT ----------
let state = null, stateAt = 0, status = 'offline', stateMsgs = 0, reconnects = 0;
const clients = new Set();
const snapshot = () => JSON.stringify({ state, status, at: stateAt, now: Date.now(), broker: broker.connected });
const broadcast = () => { const msg = `data: ${snapshot()}\n\n`; for (const c of clients) c.write(msg); };
const online = () => status === 'online' && Date.now() - stateAt < STALE_MS;

const broker = mqtt.connect(env.MQTT_URL || 'mqtt://localhost:1883', {
  username: env.MQTT_USER, password: env.MQTT_PASS,
  // Unique per process: two instances with one ID would keep kicking each other off the broker.
  clientId: `garden-webapp-${randomBytes(3).toString('hex')}`, reconnectPeriod: 5000,
});
broker.on('connect', () => { broker.subscribe(['garden/state', 'garden/status'], { qos: 1 }); broadcast(); });
broker.on('close', broadcast);
broker.on('reconnect', () => reconnects++);
broker.on('error', e => console.error('mqtt:', e.message));
broker.on('message', (topic, buf) => {
  const text = buf.toString();
  if (topic === 'garden/status') status = text === 'online' ? 'online' : 'offline';
  else if (topic === 'garden/state') {
    let s;
    try { s = JSON.parse(text); } catch { return console.warn('bad garden/state payload'); }
    if (typeof s?.moisture !== 'number') return;
    state = s; stateAt = Date.now(); stateMsgs++;
    upsert.run(Math.floor(stateAt / 60_000) * 60, s.moisture, s.raw | 0, s.pump ? 1 : 0,
      typeof s.water === 'number' ? s.water : null);
  }
  broadcast();
});

// ---------- HTTP ----------
const app = Fastify({ logger: { level: 'warn' }, bodyLimit: 1024 });
app.removeContentTypeParser('text/plain'); // only JSON bodies -> cross-site form posts get 415 (CSRF guard)

const COOKIE = 'garden_session';
const cookieAttrs = 'Path=/; HttpOnly; Secure; SameSite=Strict';
const loggedIn = req => AUTH_DISABLED || validSession(req.headers.cookie?.match(/(?:^|;\s*)garden_session=([^;]+)/)?.[1], env.SESSION_SECRET);

app.addHook('onRequest', async (req, reply) => {
  const route = req.routeOptions.url;
  if (route === '/login' || route === '/logout' || route === '/ui.css' || loggedIn(req)) return;
  if (req.method === 'GET' && route === '/') return reply.redirect('/login');
  return reply.code(401).send({ error: 'login required' });
});

// ponytail: global failed-login limit, not per IP (behind Cloudflare the IP is the edge's).
// Someone could block *new* logins for 15 min; existing sessions keep working.
let fails = [];
app.post('/login', async (req, reply) => {
  fails = fails.filter(t => Date.now() - t < 15 * 60_000);
  if (fails.length >= 10) return reply.code(429).send({ error: 'Too many attempts. Try again in 15 minutes.' });
  const { username, password } = req.body || {};
  const passOk = checkPassword(password, env.ADMIN_PASS_HASH); // always run scrypt: no timing hint about the username
  if (!(passOk && username === env.ADMIN_USER)) {
    fails.push(Date.now());
    return reply.code(401).send({ error: 'Wrong username or password' });
  }
  reply.header('Set-Cookie', `${COOKIE}=${makeSession(env.SESSION_SECRET)}; Max-Age=${SESSION_DAYS * 86_400}; ${cookieAttrs}`);
  return { ok: true };
});
app.post('/logout', (req, reply) => {
  reply.header('Set-Cookie', `${COOKIE}=; Max-Age=0; ${cookieAttrs}`);
  return { ok: true };
});

const page = name => readFileSync(new URL(`./public/${name}`, import.meta.url));
const indexHtml = page('index.html'), loginHtml = page('login.html'), uiCss = page('ui.css');
app.get('/', (req, reply) => reply.type('text/html; charset=utf-8').send(indexHtml));
app.get('/ui.css', (req, reply) => reply.type('text/css; charset=utf-8').header('Cache-Control', 'no-cache').send(uiCss));
app.get('/login', (req, reply) => loggedIn(req) ? reply.redirect('/') : reply.type('text/html; charset=utf-8').send(loginHtml));

app.get('/events', (req, reply) => {
  reply.hijack();
  reply.raw.writeHead(200, {
    // no-transform: stops Cloudflare/nginx from compressing (and so buffering) the stream.
    'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache, no-transform',
    Connection: 'keep-alive', 'X-Accel-Buffering': 'no',
  });
  reply.raw.write(`retry: 3000\ndata: ${snapshot()}\n\n`);
  clients.add(reply.raw);
  req.raw.on('close', () => clients.delete(reply.raw));
});
// Keep-alive comment so Cloudflare (100 s idle timeout) and nginx keep the stream open.
setInterval(() => { for (const c of clients) c.write(': ping\n\n'); }, 25_000).unref();

// Nerd mode: server-side numbers behind the dashboard. Polled every 5 s only while nerd mode is on.
const historyStats = db.prepare('SELECT count(*) AS rows, min(ts) AS oldest FROM history');
app.get('/api/nerd', () => {
  const mem = process.memoryUsage();
  return {
    node: process.version, uptime: Math.round(process.uptime()), rss: mem.rss, heap: mem.heapUsed,
    sseClients: clients.size, mqtt: broker.connected, clientId: broker.options.clientId, reconnects,
    statusTopic: status, stateMsgs, ...historyStats.get(),
    dbBytes: db.pragma('page_count', { simple: true }) * db.pragma('page_size', { simple: true }),
  };
});

app.get('/api/history', (req, reply) => {
  const r = RANGES[req.query.range] || RANGES['24h'];
  return historyQuery.all({ bucket: r.bucket, since: Math.floor(Date.now() / 1000) - r.secs });
});

app.post('/api/cmd/:name', async (req, reply) => {
  const cmd = COMMANDS[req.params.name];
  if (!cmd) return reply.code(404).send({ error: 'unknown command' });
  const value = cmd.parse(req.body?.value);
  if (value === null) return reply.code(400).send({ error: 'invalid value' });
  if (!broker.connected) return reply.code(503).send({ error: 'broker not connected' });
  if (!online()) return reply.code(409).send({ error: 'device offline' });
  if (req.params.name === 'pump' && value === 'on') {
    if (state.auto) return reply.code(409).send({ error: 'switch to manual mode first' });
    if (state.fault) return reply.code(409).send({ error: 'sensor fault, pump blocked' });
    if (state.locked) return reply.code(409).send({ error: 'safety lockout, reset the ESP32' });
    if (state.tankLow) return reply.code(409).send({ error: 'water tank low, refill it first' });
  }
  await broker.publishAsync(`garden/cmd/${req.params.name}`, value, { qos: 1, retain: cmd.retain });
  return { ok: true };
});

await app.listen({ host: env.HOST || '127.0.0.1', port: +(env.PORT || 3000) });
console.log(`garden dashboard on http://${env.HOST || '127.0.0.1'}:${env.PORT || 3000}`);
