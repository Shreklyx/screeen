// Screen Share – static site + WebSocket signaling + TURN credentials.
//
// The browser pages talk to this server only to set up connections; the
// screen video and audio go directly between the browsers (WebRTC), or via
// the TURN relay when a direct path is not possible.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';

const PUBLIC_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'public');
const STATIC_FILES = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/index.html': ['index.html', 'text/html; charset=utf-8'],
  '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
  '/style.css': ['style.css', 'text/css; charset=utf-8'],
};

const CODE_ALPHABET = 'abcdefghjkmnpqrstuvwxyz23456789';
const CODE_RE = /^[a-z0-9]{4,12}$/;
const CODECS = ['AV1', 'VP9', 'H264', 'VP8'];

const LIMITS = {
  rooms: 100,
  viewersPerRoom: 20,
  hostGraceMs: 2 * 60 * 1000, // the room survives a host reload / network blip this long
  authFailures: 10, // wrong passwords per IP …
  authWindowMs: 15 * 60 * 1000, // … per 15 minutes
  joins: 30, // join attempts per IP …
  joinWindowMs: 60 * 1000, // … per minute
  msgBurst: 600, // messages a socket may send in a burst (ICE candidates come in bursts)
  msgPerSec: 120,
  maxPayload: 64 * 1024,
  turnTtlSec: 24 * 3600,
};

export function loadConfig(env = process.env) {
  return {
    port: Number(env.PORT) || 8080,
    host: env.HOST || undefined,
    hostPassword: env.HOST_PASSWORD || '',
    turnSecret: env.TURN_SECRET || '',
    turnHost: env.TURN_HOST || env.DOMAIN || '',
    turnPort: Number(env.TURN_PORT) || 3478,
    trustProxy: /^(1|true|yes)$/i.test(env.TRUST_PROXY || ''),
  };
}

/* ------------------------------------------------------------ helpers */

const sha256 = (s) => crypto.createHash('sha256').update(String(s)).digest();
const passwordMatches = (given, expected) => crypto.timingSafeEqual(sha256(given), sha256(expected));

function randomCode(n = 6) {
  const bytes = crypto.randomBytes(n);
  return Array.from(bytes, (b) => CODE_ALPHABET[b % CODE_ALPHABET.length]).join('');
}

/** Sliding-window counter per key (IP). */
class RateLimiter {
  constructor(max, windowMs) {
    this.max = max;
    this.windowMs = windowMs;
    this.hits = new Map();
  }
  count(key, now = Date.now()) {
    const list = (this.hits.get(key) || []).filter((t) => now - t < this.windowMs);
    this.hits.set(key, list);
    return list.length;
  }
  limited(key) {
    return this.count(key) >= this.max;
  }
  hit(key) {
    this.count(key);
    this.hits.get(key).push(Date.now());
  }
  sweep() {
    const now = Date.now();
    for (const [key, list] of this.hits) {
      if (!list.some((t) => now - t < this.windowMs)) this.hits.delete(key);
    }
  }
}

// TURN REST API style short-lived credentials (coturn --use-auth-secret).
// A unique name per client keeps coturn's per-user allocation quota per client.
export function turnCredentials(secret, ttlSec = LIMITS.turnTtlSec, now = Date.now(), name = crypto.randomBytes(6).toString('hex')) {
  const username = `${Math.floor(now / 1000) + ttlSec}:${name}`;
  const credential = crypto.createHmac('sha1', secret).update(username).digest('base64');
  return { username, credential };
}

function iceServers(config) {
  const list = [];
  if (config.turnSecret && config.turnHost) {
    const { username, credential } = turnCredentials(config.turnSecret);
    const hostPort = `${config.turnHost}:${config.turnPort}`;
    list.push({ urls: [`stun:${hostPort}`] });
    list.push({ urls: [`turn:${hostPort}?transport=udp`, `turn:${hostPort}?transport=tcp`], username, credential });
    list.push({ urls: ['stun:stun.l.google.com:19302'] });
  } else {
    list.push({ urls: ['stun:stun.l.google.com:19302', 'stun:stun.cloudflare.com:3478'] });
  }
  return list;
}

function clientIp(req, trustProxy) {
  if (trustProxy) {
    const xff = String(req.headers['x-forwarded-for'] || '')
      .split(',')
      .map((s) => s.trim())
      .filter(Boolean);
    if (xff.length) return xff[xff.length - 1];
  }
  return req.socket.remoteAddress || 'unknown';
}

function sanitizeCaps(caps) {
  if (!caps || typeof caps !== 'object') return null;
  const out = {};
  for (const c of CODECS) {
    const v = caps[c];
    if (v && typeof v === 'object') out[c] = { supported: v.supported !== false, hw: v.hw === true };
  }
  return out;
}

/* ------------------------------------------------------------ static files */

function securityHeaders(req) {
  const host = String(req.headers.host || '').replace(/[^a-zA-Z0-9.:[\]-]/g, '');
  return {
    'Content-Security-Policy': [
      "default-src 'self'",
      "script-src 'self'",
      "style-src 'self'",
      "img-src 'self' data:",
      `connect-src 'self' wss://${host} ws://${host}`,
      "media-src 'self' blob:",
      "base-uri 'none'",
      "form-action 'self'",
      "frame-ancestors 'none'",
    ].join('; '),
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'no-referrer',
    'X-Frame-Options': 'DENY',
    'Permissions-Policy': 'display-capture=(self), camera=(), microphone=(), geolocation=()',
  };
}

function serveStatic(req, res) {
  const url = new URL(req.url, 'http://x');
  if (url.pathname === '/healthz') {
    res.writeHead(200, { 'Content-Type': 'text/plain', 'Cache-Control': 'no-store' });
    res.end('ok');
    return;
  }
  const entry = STATIC_FILES[url.pathname];
  if (!entry || (req.method !== 'GET' && req.method !== 'HEAD')) {
    res.writeHead(entry ? 405 : 404, { 'Content-Type': 'text/plain; charset=utf-8', ...securityHeaders(req) });
    res.end(entry ? 'Method not allowed' : 'Nincs ilyen oldal');
    return;
  }
  const file = path.join(PUBLIC_DIR, entry[0]);
  fs.stat(file, (err, stat) => {
    if (err) {
      res.writeHead(500, { 'Content-Type': 'text/plain' });
      res.end('Hiányzó fájl');
      return;
    }
    const etag = `"${stat.size.toString(16)}-${Math.floor(stat.mtimeMs).toString(16)}"`;
    const headers = { 'Content-Type': entry[1], 'Cache-Control': 'no-cache', ETag: etag, ...securityHeaders(req) };
    if (req.headers['if-none-match'] === etag) {
      res.writeHead(304, headers);
      res.end();
      return;
    }
    res.writeHead(200, { ...headers, 'Content-Length': stat.size });
    if (req.method === 'HEAD') {
      res.end();
      return;
    }
    fs.createReadStream(file).pipe(res);
  });
}

/* ------------------------------------------------------------ signaling */

export function createApp(config = loadConfig()) {
  const rooms = new Map();
  const authLimiter = new RateLimiter(LIMITS.authFailures, LIMITS.authWindowMs);
  const joinLimiter = new RateLimiter(LIMITS.joins, LIMITS.joinWindowMs);
  let nextId = 1;

  if (!config.hostPassword) console.warn('[screenshare] HOST_PASSWORD nincs beállítva – bárki indíthat megosztást.');
  if (!config.turnSecret) console.warn('[screenshare] TURN_SECRET nincs beállítva – TURN relé nélkül fut.');

  const server = http.createServer(serveStatic);
  const wss = new WebSocketServer({
    server,
    path: '/ws',
    maxPayload: LIMITS.maxPayload,
    verifyClient: ({ origin, req }) => {
      // Browsers always send Origin: only accept pages served from this host.
      if (!origin) return true;
      try {
        return new URL(origin).host === req.headers.host;
      } catch {
        return false;
      }
    },
  });

  const send = (conn, msg) => {
    if (conn.ws.readyState === conn.ws.OPEN) conn.ws.send(JSON.stringify(msg));
  };

  const broadcastCount = (room) => {
    const msg = { t: 'count', n: room.viewers.size };
    if (room.host) send(room.host, msg);
    for (const v of room.viewers.values()) send(v, msg);
  };

  function closeRoom(room, reason = 'closed') {
    clearTimeout(room.awayTimer);
    rooms.delete(room.code);
    for (const v of room.viewers.values()) {
      v.room = null;
      v.role = null;
      send(v, { t: 'closed', reason });
    }
    room.viewers.clear();
    if (room.host) {
      room.host.room = null;
      room.host.role = null;
    }
  }

  function leave(conn) {
    const room = conn.room;
    if (!room) return;
    if (conn.role === 'viewer') {
      room.viewers.delete(conn.id);
      if (room.host) send(room.host, { t: 'viewer-left', id: conn.id });
      broadcastCount(room);
    } else if (conn.role === 'host' && room.host === conn) {
      room.host = null;
      for (const v of room.viewers.values()) send(v, { t: 'host-away' });
      room.awayTimer = setTimeout(() => closeRoom(room, 'timeout'), LIMITS.hostGraceMs);
    }
    conn.room = null;
    conn.role = null;
  }

  function attachHost(conn, room) {
    clearTimeout(room.awayTimer);
    const previous = room.host;
    room.host = conn;
    conn.room = room;
    conn.role = 'host';
    if (previous && previous !== conn) {
      previous.room = null;
      previous.role = null;
      send(previous, { t: 'replaced' });
    }
    send(conn, {
      t: 'hosted',
      code: room.code,
      token: room.token,
      iceServers: iceServers(config),
      live: room.live,
      viewers: [...room.viewers.values()].map((v) => ({ id: v.id, caps: v.caps })),
    });
    for (const v of room.viewers.values()) send(v, { t: 'host-back', live: room.live });
    broadcastCount(room);
  }

  const handlers = {
    host(conn, msg) {
      const resume = msg.resume;
      if (resume && typeof resume === 'object') {
        const room = rooms.get(String(resume.code || ''));
        if (room && typeof resume.token === 'string' && passwordMatches(resume.token, room.token)) {
          if (conn.room && conn.room !== room) leave(conn);
          attachHost(conn, room);
          return;
        }
      }
      if (config.hostPassword) {
        if (authLimiter.limited(conn.ip)) return send(conn, { t: 'error', code: 'rate' });
        if (typeof msg.password !== 'string' || !passwordMatches(msg.password, config.hostPassword)) {
          authLimiter.hit(conn.ip);
          return send(conn, { t: 'error', code: 'auth' });
        }
      }
      if (rooms.size >= LIMITS.rooms) return send(conn, { t: 'error', code: 'full' });
      if (conn.room) leave(conn);
      let code;
      do code = randomCode(); while (rooms.has(code));
      const room = {
        code,
        token: crypto.randomBytes(24).toString('base64url'),
        host: null,
        viewers: new Map(),
        live: false,
        awayTimer: null,
      };
      rooms.set(code, room);
      attachHost(conn, room);
    },

    join(conn, msg) {
      if (joinLimiter.limited(conn.ip)) return send(conn, { t: 'error', code: 'rate' });
      joinLimiter.hit(conn.ip);
      const code = String(msg.code || '').toLowerCase();
      const room = CODE_RE.test(code) ? rooms.get(code) : null;
      if (!room) return send(conn, { t: 'error', code: 'notfound' });
      if (room.viewers.size >= LIMITS.viewersPerRoom) return send(conn, { t: 'error', code: 'full' });
      if (conn.room) leave(conn);
      conn.role = 'viewer';
      conn.room = room;
      conn.caps = sanitizeCaps(msg.caps);
      room.viewers.set(conn.id, conn);
      send(conn, { t: 'joined', id: conn.id, code: room.code, iceServers: iceServers(config), live: room.live, hostPresent: Boolean(room.host) });
      if (room.host) send(room.host, { t: 'viewer', id: conn.id, caps: conn.caps });
      broadcastCount(room);
    },

    signal(conn, msg) {
      const room = conn.room;
      if (!room || !msg.data || typeof msg.data !== 'object') return;
      if (conn.role === 'host' && room.host === conn) {
        const target = room.viewers.get(msg.to);
        if (target) send(target, { t: 'signal', from: 'host', data: msg.data });
      } else if (conn.role === 'viewer' && msg.to === 'host' && room.host) {
        send(room.host, { t: 'signal', from: conn.id, data: msg.data });
      }
    },

    state(conn, msg) {
      const room = conn.room;
      if (!room || conn.role !== 'host' || room.host !== conn) return;
      room.live = Boolean(msg.live);
      for (const v of room.viewers.values()) send(v, { t: 'state', live: room.live });
    },

    close(conn) {
      const room = conn.room;
      if (room && conn.role === 'host' && room.host === conn) closeRoom(room);
    },

    leave(conn) {
      if (conn.role === 'viewer') leave(conn);
    },
  };

  wss.on('connection', (ws, req) => {
    const conn = {
      id: `v${nextId++}`,
      ws,
      ip: clientIp(req, config.trustProxy),
      room: null,
      role: null,
      caps: null,
      tokens: LIMITS.msgBurst,
      lastRefill: Date.now(),
    };

    ws.isAlive = true;
    ws.on('pong', () => {
      ws.isAlive = true;
    });

    ws.on('message', (raw, isBinary) => {
      const now = Date.now();
      conn.tokens = Math.min(LIMITS.msgBurst, conn.tokens + ((now - conn.lastRefill) / 1000) * LIMITS.msgPerSec);
      conn.lastRefill = now;
      if (conn.tokens < 1) return; // flooding – drop
      conn.tokens -= 1;
      if (isBinary) return;
      let msg;
      try {
        msg = JSON.parse(raw.toString());
      } catch {
        return;
      }
      if (!msg || typeof msg !== 'object' || typeof msg.t !== 'string' || !Object.hasOwn(handlers, msg.t)) return;
      try {
        handlers[msg.t](conn, msg);
      } catch (err) {
        console.error('[screenshare] handler error', err);
      }
    });

    ws.on('close', () => leave(conn));
    ws.on('error', () => {});
  });

  // Drop dead connections (closed laptops, lost Wi-Fi) so rooms stay accurate.
  const heartbeat = setInterval(() => {
    for (const ws of wss.clients) {
      if (!ws.isAlive) {
        ws.terminate();
        continue;
      }
      ws.isAlive = false;
      ws.ping();
    }
    authLimiter.sweep();
    joinLimiter.sweep();
  }, 30000);
  heartbeat.unref();

  return {
    server,
    wss,
    rooms,
    close() {
      clearInterval(heartbeat);
      for (const room of rooms.values()) clearTimeout(room.awayTimer);
      for (const ws of wss.clients) ws.terminate();
      wss.close();
      return new Promise((resolve) => server.close(() => resolve()));
    },
  };
}

/* ------------------------------------------------------------ main */

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const config = loadConfig();
  const app = createApp(config);
  app.server.listen(config.port, config.host, () => {
    console.log(`[screenshare] fut: http://localhost:${config.port}`);
  });
  const shutdown = () => app.close().then(() => process.exit(0));
  process.on('SIGTERM', shutdown);
  process.on('SIGINT', shutdown);
}
