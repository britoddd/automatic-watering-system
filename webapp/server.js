// Garden dashboard backend: one MQTT connection, SQLite history, SSE to browsers.
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const env = process.env;
const STALE_MS = 30_000;

// ---------- Command validation (server is the trust boundary) ----------
const COMMANDS = {
  mode:      { retain: true,  parse: v => (v === 'auto' || v === 'manual') ? v : null },
  threshold: { retain: true,  parse: v => /^\d{1,2}$/.test(String(v)) && +v >= 5 && +v <= 60 ? String(+v) : null },
  pump:      { retain: false, parse: v => (v === 'on' || v === 'off') ? v : null }, // NEVER retain pump
};

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
  console.log('selftest ok');
  process.exit(0);
}

const { default: Fastify } = await import('fastify');
const { default: mqtt } = await import('mqtt');
const { default: Database } = await import('better-sqlite3');
const { createRemoteJWKSet, jwtVerify } = await import('jose');

// ---------- SQLite history: one row per minute ----------
const db = new Database(env.DB_PATH || 'garden.db');
db.pragma('journal_mode = WAL');
db.exec(`CREATE TABLE IF NOT EXISTS history (
  ts INTEGER PRIMARY KEY,  -- unix seconds, start of minute
  moisture INTEGER, raw INTEGER,
  pump INTEGER             -- 1 if the pump was on at any point in this minute
)`);
// Upsert every state message into its minute bucket; max() keeps short pump bursts visible.
const upsert = db.prepare(`INSERT INTO history (ts, moisture, raw, pump) VALUES (?, ?, ?, ?)
  ON CONFLICT(ts) DO UPDATE SET moisture = excluded.moisture, raw = excluded.raw, pump = max(pump, excluded.pump)`);
const prune = db.prepare('DELETE FROM history WHERE ts < ?');
const historyQuery = db.prepare(`SELECT (ts / @bucket) * @bucket AS t, round(avg(moisture)) AS moisture, max(pump) AS pump
  FROM history WHERE ts >= @since GROUP BY t ORDER BY t`);
const RANGES = { '24h': { secs: 86_400, bucket: 60 }, '7d': { secs: 604_800, bucket: 600 } };

const pruneOld = () => prune.run(Math.floor(Date.now() / 1000) - 30 * 86_400);
pruneOld();
setInterval(pruneOld, 3_600_000).unref();

// ---------- MQTT ----------
let state = null, stateAt = 0, status = 'offline';
const clients = new Set();
const snapshot = () => JSON.stringify({ state, status, at: stateAt, now: Date.now(), broker: broker.connected });
const broadcast = () => { const msg = `data: ${snapshot()}\n\n`; for (const c of clients) c.write(msg); };
const online = () => status === 'online' && Date.now() - stateAt < STALE_MS;

const broker = mqtt.connect(env.MQTT_URL || 'mqtt://localhost:1883', {
  username: env.MQTT_USER, password: env.MQTT_PASS,
  clientId: 'garden-webapp', reconnectPeriod: 5000,
});
broker.on('connect', () => { broker.subscribe(['garden/state', 'garden/status'], { qos: 1 }); broadcast(); });
broker.on('close', broadcast);
broker.on('error', e => console.error('mqtt:', e.message));
broker.on('message', (topic, buf) => {
  const text = buf.toString();
  if (topic === 'garden/status') status = text === 'online' ? 'online' : 'offline';
  else if (topic === 'garden/state') {
    let s;
    try { s = JSON.parse(text); } catch { return console.warn('bad garden/state payload'); }
    if (typeof s?.moisture !== 'number') return;
    state = s; stateAt = Date.now();
    upsert.run(Math.floor(stateAt / 60_000) * 60, s.moisture, s.raw | 0, s.pump ? 1 : 0);
  }
  broadcast();
});

// ---------- HTTP ----------
const app = Fastify({ logger: { level: 'warn' }, bodyLimit: 1024 });
app.removeContentTypeParser('text/plain'); // only JSON bodies -> cross-site form posts get 415 (CSRF guard)

// Cloudflare Access: verify the signed JWT on every request so hitting the origin IP directly gets nothing.
if (env.AUTH_DISABLED === '1') {
  console.warn('WARNING: AUTH_DISABLED=1, no login check. Local testing only.');
} else {
  if (!env.CF_TEAM_DOMAIN || !env.CF_AUD) throw new Error('Set CF_TEAM_DOMAIN and CF_AUD (or AUTH_DISABLED=1 for local tests)');
  const issuer = `https://${env.CF_TEAM_DOMAIN}`;
  const jwks = createRemoteJWKSet(new URL(`${issuer}/cdn-cgi/access/certs`));
  app.addHook('onRequest', async (req, reply) => {
    try { await jwtVerify(req.headers['cf-access-jwt-assertion'] || '', jwks, { issuer, audience: env.CF_AUD }); }
    catch { return reply.code(403).send('Forbidden'); }
  });
}

const indexHtml = readFileSync(new URL('./public/index.html', import.meta.url));
app.get('/', (req, reply) => reply.type('text/html; charset=utf-8').send(indexHtml));

app.get('/events', (req, reply) => {
  reply.hijack();
  reply.raw.writeHead(200, {
    'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache',
    Connection: 'keep-alive', 'X-Accel-Buffering': 'no',
  });
  reply.raw.write(`retry: 3000\ndata: ${snapshot()}\n\n`);
  clients.add(reply.raw);
  req.raw.on('close', () => clients.delete(reply.raw));
});
// Keep-alive comment so Cloudflare (100 s idle timeout) and nginx keep the stream open.
setInterval(() => { for (const c of clients) c.write(': ping\n\n'); }, 25_000).unref();

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
  }
  await broker.publishAsync(`garden/cmd/${req.params.name}`, value, { qos: 1, retain: cmd.retain });
  return { ok: true };
});

await app.listen({ host: env.HOST || '127.0.0.1', port: +(env.PORT || 3000) });
console.log(`garden dashboard on http://${env.HOST || '127.0.0.1'}:${env.PORT || 3000}`);
