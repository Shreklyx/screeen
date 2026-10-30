// Screen Share – static site + WebSocket signaling + the direct mode's relay.
//
// No WebRTC: the picture and the sound go with our own protocol ("direct mode", see
// direct-relay.js), encrypted end to end. The apps send to each other directly over UDP – the
// server's UDP port tells them their public address and relays if needed (or the TURN server);
// browsers send through the /direct WebSocket, which the server passes on.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';
import { createRelay } from './direct-relay.js';

const PUBLIC_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'public');
const STATIC_FILES = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/index.html': ['index.html', 'text/html; charset=utf-8'],
  '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
  '/style.css': ['style.css', 'text/css; charset=utf-8'],
  '/direct-audio.js': ['direct-audio.js', 'text/javascript; charset=utf-8'], // direct mode: audio playback worklet
  '/direct-web.js': ['direct-web.js', 'text/javascript; charset=utf-8'], // direct mode in the browser (WebSocket)
  '/capture-audio.js': ['capture-audio.js', 'text/javascript; charset=utf-8'], // sharing sound without MediaStreamTrackProcessor
  // our C/C++ code as WebAssembly: X25519, an Opus decoder (native/web)
  '/ss-web.js': ['ss-web.js', 'text/javascript; charset=utf-8'],
  '/ss-web.wasm': ['ss-web.wasm', 'application/wasm'],
  // home-screen app (iPhone / iPad / Android)
  '/manifest.webmanifest': ['manifest.webmanifest', 'application/manifest+json; charset=utf-8'],
  '/apple-touch-icon.png': ['apple-touch-icon.png', 'image/png'],
  '/apple-touch-icon-precomposed.png': ['apple-touch-icon.png', 'image/png'],
  '/icon-192.png': ['icon-192.png', 'image/png'],
  '/icon-512.png': ['icon-512.png', 'image/png'],
  '/icon-maskable-512.png': ['icon-maskable-512.png', 'image/png'],
};

const APP_ORIGIN = 'app://screenshare'; // the desktop app (desktop/main.js)
const CODE_ALPHABET = 'abcdefghjkmnpqrstuvwxyz23456789';
const CODE_RE = /^[a-z0-9]{4,12}$/;
// what the two ends may tell each other (no WebRTC offers / answers / ICE candidates any more)
const HOST_SIGNALS = new Set(['stats', 'direct-offer', 'unsupported']);
const VIEWER_SIGNALS = new Set(['recall', 'stats', 'direct-answer', 'direct-fail']);

const LIMITS = {
  rooms: 100,
  viewersPerRoom: 20,
  hostGraceMs: 10 * 60 * 1000, // the room waits this long for its sharer (a reload, a restart, the computer's) – the apps go back into it
  authFailures: 10, // wrong passwords per IP …
  authWindowMs: 15 * 60 * 1000, // … per 15 minutes
  joins: 30, // join attempts per IP …
  joinWindowMs: 60 * 1000, // … per minute
  hostBurst: 600, // messages a host may send in a burst (every viewer's connection set up at once)
  hostPerSec: 120,
  viewerBurst: 100, // everyone else (viewers, sockets that have not joined yet)
  viewerPerSec: 20,
  maxPayload: 64 * 1024,
  maxSignal: 32 * 1024, // one signal (keys, addresses, statistics)
  maxBuffered: 1 << 20, // skip sending to a socket that does not keep up
  turnTtlSec: 24 * 3600,
  // the room's chat
  chatText: 500, // characters in a message
  chatName: 24, // characters in a name
  chatHistory: 50, // messages a room keeps for those who come later
  chatBurst: 5, // messages one person may send at once …
  chatPerSec: 1, // … and then this many a second
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
    // direct mode (app ↔ app): UDP port for address discovery and relaying; 0 = off
    directPort: Number(env.DIRECT_PORT) || 0,
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

/** What people type (a chat message, a name): no control characters or bidi overrides (text that
 *  turns itself around), single spaces, at most `max` characters. */
export function cleanText(value, max) {
  if (typeof value !== 'string') return '';
  const s = value.normalize('NFC').replace(/[\p{Cc}\u200E\u200F\u202A-\u202E\u2066-\u2069]/gu, ' ').replace(/\s+/g, ' ').trim();
  return [...s].slice(0, max).join('').trim();
}

function sanitizeCaps(caps) {
  if (!caps || typeof caps !== 'object') return null;
  const out = {};
  if (caps.surround === true) out.surround = true; // 5.1/7.1 speakers
  if (caps.direct === true) out.direct = true; // speaks our protocol: an app, or a browser with WebCodecs
  return out;
}

/* ------------------------------------------------------------ static files */

function securityHeaders(req) {
  const host = String(req.headers.host || '').replace(/[^a-zA-Z0-9.:[\]-]/g, '');
  return {
    'Content-Security-Policy': [
      "default-src 'self'",
      "script-src 'self' 'wasm-unsafe-eval'", // our WebAssembly (ss-web.wasm)
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
    // microphone: an audio input can be the shared sound source (e.g. BlackHole on a Mac)
    'Permissions-Policy': 'display-capture=(self), microphone=(self), camera=(), geolocation=()',
  };
}

// The few page files are small: keep them in memory, re-read only when they change on disk.
const fileCache = new Map();
function loadFile(name) {
  const file = path.join(PUBLIC_DIR, name);
  const stat = fs.statSync(file);
  const cached = fileCache.get(name);
  if (cached && cached.mtimeMs === stat.mtimeMs && cached.size === stat.size) return cached;
  const body = fs.readFileSync(file);
  const entry = {
    body,
    mtimeMs: stat.mtimeMs,
    size: stat.size,
    etag: `"${crypto.createHash('sha1').update(body).digest('base64url').slice(0, 16)}"`,
  };
  fileCache.set(name, entry);
  return entry;
}

function serveStatic(req, res) {
  try {
    handleStatic(req, res);
  } catch (err) {
    console.error('[screenshare] request error', err);
    if (!res.headersSent) res.writeHead(500, { 'Content-Type': 'text/plain' });
    res.end();
  }
}

function handleStatic(req, res) {
  let url;
  try {
    url = new URL(req.url, 'http://x');
  } catch {
    res.writeHead(400, { 'Content-Type': 'text/plain' });
    res.end('Bad request');
    return;
  }
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
  const file = loadFile(entry[0]);
  const headers = { 'Content-Type': entry[1], 'Cache-Control': 'no-cache', ETag: file.etag, ...securityHeaders(req) };
  if (req.headers['if-none-match'] === file.etag) {
    res.writeHead(304, headers);
    res.end();
    return;
  }
  res.writeHead(200, { ...headers, 'Content-Length': file.body.length });
  res.end(req.method === 'HEAD' ? undefined : file.body);
}

/* ------------------------------------------------------------ signaling */

export function createApp(config = loadConfig()) {
  const rooms = new Map();
  let closing = false;
  // direct mode sessions; the UDP port only when DIRECT_PORT is set (the TCP fallback always works)
  const relay = createRelay({ port: config.directPort, host: config.host });
  const relayReady = config.directPort ? relay.listen().catch((err) => {
    console.error(`[screenshare] direct mode UDP port ${config.directPort} unavailable: ${err.message}`);
    return null;
  }) : Promise.resolve(null);
  const authLimiter = new RateLimiter(LIMITS.authFailures, LIMITS.authWindowMs);
  const joinLimiter = new RateLimiter(LIMITS.joins, LIMITS.joinWindowMs);
  let nextId = 1;

  if (!config.hostPassword) console.warn('[screenshare] HOST_PASSWORD nincs beállítva – bárki indíthat megosztást.');
  if (!config.turnSecret) console.warn('[screenshare] TURN_SECRET nincs beállítva – TURN relé nélkül fut.');

  const server = http.createServer(serveStatic);
  const wss = new WebSocketServer({
    noServer: true,
    maxPayload: LIMITS.maxPayload,
    verifyClient: ({ origin, req }) => {
      // Browsers always send Origin: only accept pages served from this host – and the desktop
      // app's built-in pages (a scheme no website can have).
      if (!origin || origin === APP_ORIGIN) return true;
      try {
        return new URL(origin).host === req.headers.host;
      } catch {
        return false;
      }
    },
  });

  // direct mode over the HTTPS port: browsers, and apps without UDP (see direct-relay.js);
  // a frame goes as one message there (a 4K keyframe can be a few MB)
  const directWss = new WebSocketServer({ noServer: true, maxPayload: 16 * 1024 * 1024 });
  server.on('upgrade', (req, socket, head) => {
    let pathname = '';
    try {
      pathname = new URL(req.url, 'http://x').pathname;
    } catch {
      // handled below
    }
    if (pathname === '/ws') wss.handleUpgrade(req, socket, head, (ws) => wss.emit('connection', ws, req));
    else if (pathname === '/direct') directWss.handleUpgrade(req, socket, head, (ws) => relay.acceptTcp(ws));
    else socket.destroy();
  });

  const send = (conn, msg) => {
    if (conn.ws.readyState === conn.ws.OPEN && conn.ws.bufferedAmount < LIMITS.maxBuffered) conn.ws.send(JSON.stringify(msg));
  };

  const broadcastCount = (room) => {
    const msg = { t: 'count', n: room.viewers.size };
    if (room.host) send(room.host, msg);
    for (const v of room.viewers.values()) send(v, msg);
  };

  function closeRoom(room, reason = 'closed') {
    clearTimeout(room.awayTimer);
    rooms.delete(room.code);
    relay.removeWhere((s) => s.room === room.code);
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
      relay.removeWhere((s) => s.room === room.code && s.viewer === conn.id);
      if (room.host) send(room.host, { t: 'viewer-left', id: conn.id });
      broadcastCount(room);
    } else if (conn.role === 'host' && room.host === conn) {
      room.host = null;
      for (const v of room.viewers.values()) send(v, { t: 'host-away' });
      if (!closing) room.awayTimer = setTimeout(() => closeRoom(room, 'timeout'), LIMITS.hostGraceMs);
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
      viewers: [...room.viewers.values()].map((v) => ({ id: v.id, caps: v.caps, name: v.name })),
      direct: true, // this server sets up the direct mode
      name: conn.name,
      chat: room.chat,
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
          conn.name = cleanText(msg.name, LIMITS.chatName) || room.host?.name || 'Megosztó';
          attachHost(conn, room);
          return;
        }
      }
      if (config.hostPassword) {
        if (typeof msg.password !== 'string' || !msg.password) return send(conn, { t: 'error', code: 'auth' });
        if (authLimiter.limited(conn.ip)) return send(conn, { t: 'error', code: 'rate' });
        if (!passwordMatches(msg.password, config.hostPassword)) {
          authLimiter.hit(conn.ip);
          return send(conn, { t: 'error', code: 'auth' });
        }
      }
      if (rooms.size >= LIMITS.rooms) return send(conn, { t: 'error', code: 'full' });
      if (conn.room) leave(conn);
      conn.name = cleanText(msg.name, LIMITS.chatName) || 'Megosztó';
      let code;
      do code = randomCode(); while (rooms.has(code));
      const room = {
        code,
        token: crypto.randomBytes(24).toString('base64url'),
        host: null,
        viewers: new Map(),
        live: false,
        awayTimer: null,
        chat: [], // the last messages: {id, from: 'host' or a viewer's id, name, text, at}
        chatSeq: 0,
        viewerSeq: 0, // for the names of those who give none: Néző 1, Néző 2, …
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
      conn.name = cleanText(msg.name, LIMITS.chatName) || `Néző ${++room.viewerSeq}`;
      room.viewers.set(conn.id, conn);
      send(conn, {
        t: 'joined',
        id: conn.id,
        code: room.code,
        iceServers: iceServers(config),
        live: room.live,
        hostPresent: Boolean(room.host),
        name: conn.name,
        chat: room.chat,
      });
      if (room.host) send(room.host, { t: 'viewer', id: conn.id, caps: conn.caps, name: conn.name });
      broadcastCount(room);
    },

    signal(conn, msg, size) {
      const room = conn.room;
      const data = msg.data;
      if (!room || !data || typeof data !== 'object' || size > LIMITS.maxSignal) return;
      if (conn.role === 'host' && room.host === conn) {
        const target = room.viewers.get(msg.to);
        if (target && HOST_SIGNALS.has(data.type)) send(target, { t: 'signal', from: 'host', data });
      } else if (conn.role === 'viewer' && msg.to === 'host' && room.host && VIEWER_SIGNALS.has(data.type)) {
        send(room.host, { t: 'signal', from: conn.id, data });
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

    // the room's chat: everyone in it – the sharer and every viewer – gets every message (the sender
    // too: that is the delivery), with the name the server knows the sender by
    chat(conn, msg) {
      const room = conn.room;
      if (!room || (conn.role === 'host' && room.host !== conn)) return;
      const text = cleanText(msg.text, LIMITS.chatText);
      if (!text) return;
      const now = Date.now();
      conn.chatTokens = Math.min(LIMITS.chatBurst, conn.chatTokens + ((now - conn.chatRefill) / 1000) * LIMITS.chatPerSec);
      conn.chatRefill = now;
      if (conn.chatTokens < 1) return send(conn, { t: 'chat-limit' });
      conn.chatTokens -= 1;
      const entry = { id: ++room.chatSeq, from: conn.role === 'host' ? 'host' : conn.id, name: conn.name, text, at: now };
      room.chat.push(entry);
      if (room.chat.length > LIMITS.chatHistory) room.chat.shift();
      const out = { t: 'chat', ...entry };
      if (room.host) send(room.host, out);
      for (const v of room.viewers.values()) send(v, out);
    },

    name(conn, msg) {
      const room = conn.room;
      const name = cleanText(msg.name, LIMITS.chatName);
      if (!room || !name || (conn.role === 'host' && room.host !== conn)) return;
      conn.name = name;
      if (conn.role === 'viewer' && room.host) send(room.host, { t: 'viewer-name', id: conn.id, name });
    },

    // direct mode for one viewer: a session (id + secret) for the UDP registration / relay
    direct(conn, msg) {
      const room = conn.room;
      if (!room || conn.role !== 'host' || room.host !== conn) return;
      const target = room.viewers.get(msg.to);
      if (!target || !target.caps?.direct) return;
      relay.removeWhere((s) => s.room === room.code && s.viewer === target.id);
      const { session, secret } = relay.create({ room: room.code, viewer: target.id });
      const info = { t: 'direct', session, secret, port: config.directPort };
      send(conn, { ...info, viewer: target.id });
      send(target, info);
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
      name: '',
      tokens: LIMITS.viewerBurst,
      lastRefill: Date.now(),
      chatTokens: LIMITS.chatBurst,
      chatRefill: Date.now(),
    };

    ws.isAlive = true;
    ws.on('pong', () => {
      ws.isAlive = true;
    });

    ws.on('message', (raw, isBinary) => {
      const now = Date.now();
      const [burst, perSec] = conn.role === 'host' ? [LIMITS.hostBurst, LIMITS.hostPerSec] : [LIMITS.viewerBurst, LIMITS.viewerPerSec];
      conn.tokens = Math.min(burst, conn.tokens + ((now - conn.lastRefill) / 1000) * perSec);
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
        handlers[msg.t](conn, msg, raw.length);
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
    relay,
    relayReady,
    close() {
      closing = true; // connections closing now must not start a room's grace timer
      clearInterval(heartbeat);
      relay.close();
      for (const ws of directWss.clients) ws.terminate();
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
