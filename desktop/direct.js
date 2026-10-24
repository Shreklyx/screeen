'use strict';

// Direct mode: the desktop app's own low-latency transport, used instead of WebRTC when
// both the sharer and the viewer run the app.
//
// One UDP socket per app window. Every packet is encrypted (AES-128-GCM, keys from an
// X25519 exchange over the signaling server, so the server cannot read the picture).
// The sharer's encoded frames are cut into ~1.1 KB packets and sent the moment they are
// ready; the viewer hands every complete frame to the decoder at once – there is no
// jitter buffer. Lost packets are asked for again (NACK); a frame that cannot be
// completed in time is skipped and the viewer asks for a keyframe.
//
// Connection: both ends register at the server's UDP port (the answer is their public
// address), exchange addresses over signaling and probe each other (UDP hole punching).
// When no direct path answers, packets go through the server's UDP relay – or, when that
// port is blocked, through the TURN server (the same one WebRTC uses, port 3478).

const dgram = require('node:dgram');
const crypto = require('node:crypto');
const dns = require('node:dns');
const os = require('node:os');
const { EventEmitter } = require('node:events');
const { performance } = require('node:perf_hooks');

const now = () => performance.timeOrigin + performance.now(); // ms, same clock as the pages

const PEER = 0x53; // 'S': packets between the two apps (the relay forwards them as they are)
const REG = 0x52; // 'R': registration at the server
const REG_ACK = 0x41; // 'A': the server's answer, with the address it saw
const T = { PROBE: 1, PROBE_ACK: 2, MEDIA: 3, NACK: 4, PLI: 5, PING: 6, PONG: 7, REPORT: 8, BYE: 9 };
const ROLE = { host: 1, viewer: 2 };
const HEADER = 14; // magic, type, session (4), counter (8) – authenticated, not encrypted
const TAG = 16;
const MEDIA_HEADER = 32;
const MTU = 1200; // UDP payload: fits every path, VPNs and PPPoE included
const MAX_DATA = MTU - HEADER - TAG - MEDIA_HEADER;
const RING = 1 << 14; // video packets kept for retransmission
const MAX_CANDS = 8;
const INFO = Buffer.from('screenshare-direct-v1');
const X25519_SPKI = Buffer.from('302a300506032b656e032100', 'hex');

/* ------------------------------------------------ crypto */

function keyPair() {
  const { publicKey, privateKey } = crypto.generateKeyPairSync('x25519');
  return { privateKey, pub: publicKey.export({ type: 'spki', format: 'der' }).subarray(-32).toString('base64') };
}

/** Keys for both directions, from our private key and the other side's public key. */
function deriveKeys(privateKey, peerPub, session) {
  const raw = Buffer.from(String(peerPub), 'base64');
  if (raw.length !== 32) throw new Error('invalid public key');
  const publicKey = crypto.createPublicKey({ key: Buffer.concat([X25519_SPKI, raw]), format: 'der', type: 'spki' });
  const shared = crypto.diffieHellman({ privateKey, publicKey });
  const salt = Buffer.alloc(4);
  salt.writeUInt32BE(session >>> 0);
  const okm = Buffer.from(crypto.hkdfSync('sha256', shared, salt, INFO, 32));
  return { toViewer: okm.subarray(0, 16), toHost: okm.subarray(16, 32) };
}

function nonce(dir, packet) {
  const iv = Buffer.alloc(12);
  iv[0] = dir;
  packet.copy(iv, 4, 6, 14);
  return iv;
}

function seal(key, dir, type, session, counter, payload) {
  const out = Buffer.allocUnsafe(HEADER + payload.length + TAG);
  out[0] = PEER;
  out[1] = type;
  out.writeUInt32BE(session >>> 0, 2);
  out.writeBigUInt64BE(counter, 6);
  const cipher = crypto.createCipheriv('aes-128-gcm', key, nonce(dir, out));
  cipher.setAAD(out.subarray(0, HEADER));
  cipher.update(payload).copy(out, HEADER);
  cipher.final();
  cipher.getAuthTag().copy(out, HEADER + payload.length);
  return out;
}

function unseal(key, dir, packet) {
  if (packet.length < HEADER + TAG) return null;
  try {
    const decipher = crypto.createDecipheriv('aes-128-gcm', key, nonce(dir, packet));
    decipher.setAAD(packet.subarray(0, HEADER));
    decipher.setAuthTag(packet.subarray(packet.length - TAG));
    const body = decipher.update(packet.subarray(HEADER, packet.length - TAG));
    decipher.final();
    return body;
  } catch {
    return null;
  }
}

const hmac16 = (secret, data) => crypto.createHmac('sha256', secret).update(data).digest().subarray(0, 16);

/** Registration packet (64 bytes, so the server's answer is never larger than the question). */
function registration(role, session, secret) {
  const b = Buffer.alloc(64);
  b[0] = REG;
  b[1] = ROLE[role];
  b.writeUInt32BE(session >>> 0, 2);
  crypto.randomFillSync(b, 6, 8);
  hmac16(secret, b.subarray(0, 14)).copy(b, 14);
  return b;
}

/** The server's answer: { session, address, port } or null. */
function parseRegistrationAck(msg, secretFor) {
  if (msg.length < 37 || msg[0] !== REG_ACK || msg[14] !== 4) return null;
  const session = msg.readUInt32BE(2);
  const secret = secretFor(session);
  if (!secret || !crypto.timingSafeEqual(hmac16(secret, msg.subarray(0, 21)), msg.subarray(21, 37))) return null;
  return { session, address: `${msg[15]}.${msg[16]}.${msg[17]}.${msg[18]}`, port: msg.readUInt16BE(19) };
}

/* ------------------------------------------------ addresses */

const IPV4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;

function isPrivate(address) {
  const m = IPV4.exec(address);
  if (!m) return false;
  const [a, b] = [Number(m[1]), Number(m[2])];
  return a === 10 || a === 127 || (a === 172 && b >= 16 && b < 32) || (a === 192 && b === 168) || (a === 169 && b === 254) || (a === 100 && b >= 64 && b < 128);
}

function localAddresses() {
  const out = [];
  for (const list of Object.values(os.networkInterfaces())) {
    for (const i of list || []) {
      if ((i.family === 'IPv4' || i.family === 4) && !i.internal && IPV4.test(i.address)) out.push(i.address);
    }
  }
  return out;
}

function validCandidate(c) {
  if (!c || typeof c !== 'object' || typeof c.address !== 'string') return false;
  const m = IPV4.exec(c.address);
  const port = Number(c.port);
  return Boolean(m) && m.slice(1).every((x) => Number(x) <= 255) && Number(m[1]) !== 0 && Number(m[1]) < 224 && Number.isInteger(port) && port > 0 && port < 65536;
}

/* ------------------------------------------------ TURN (RFC 8656), the fallback relay */

const STUN_COOKIE = 0x2112a442;
const ATTR = {
  USERNAME: 0x0006, MESSAGE_INTEGRITY: 0x0008, ERROR_CODE: 0x0009, CHANNEL_NUMBER: 0x000c, LIFETIME: 0x000d,
  XOR_PEER_ADDRESS: 0x0012, DATA: 0x0013, REALM: 0x0014, NONCE: 0x0015, XOR_RELAYED_ADDRESS: 0x0016,
  REQUESTED_TRANSPORT: 0x0019, XOR_MAPPED_ADDRESS: 0x0020,
};
const METHOD = { ALLOCATE: 0x0003, REFRESH: 0x0004, CHANNEL_BIND: 0x0009 };
const DATA_INDICATION = 0x0017;

const u32 = (n) => {
  const b = Buffer.alloc(4);
  b.writeUInt32BE(n >>> 0);
  return b;
};

function stunEncode(type, tid, attrs, key) {
  let body = Buffer.concat(attrs.map(([t, v]) => {
    const h = Buffer.alloc(4);
    h.writeUInt16BE(t, 0);
    h.writeUInt16BE(v.length, 2);
    return Buffer.concat([h, v, Buffer.alloc((4 - (v.length % 4)) % 4)]);
  }));
  const header = Buffer.alloc(20);
  header.writeUInt16BE(type, 0);
  header.writeUInt32BE(STUN_COOKIE, 4);
  tid.copy(header, 8);
  if (key) {
    // MESSAGE-INTEGRITY: HMAC-SHA1 over everything before it, the length already counting it
    header.writeUInt16BE(body.length + 24, 2);
    const mac = crypto.createHmac('sha1', key).update(Buffer.concat([header, body])).digest();
    const h = Buffer.alloc(4);
    h.writeUInt16BE(ATTR.MESSAGE_INTEGRITY, 0);
    h.writeUInt16BE(20, 2);
    body = Buffer.concat([body, h, mac]);
  }
  header.writeUInt16BE(body.length, 2);
  return Buffer.concat([header, body]);
}

function stunParse(msg) {
  if (msg.length < 20 || (msg[0] & 0xc0) !== 0 || msg.readUInt32BE(4) !== STUN_COOKIE) return null;
  const end = 20 + msg.readUInt16BE(2);
  if (end > msg.length) return null;
  const attrs = new Map();
  for (let o = 20; o + 4 <= end;) {
    const t = msg.readUInt16BE(o);
    const l = msg.readUInt16BE(o + 2);
    if (o + 4 + l > end) break;
    if (!attrs.has(t)) attrs.set(t, msg.subarray(o + 4, o + 4 + l));
    o += 4 + l + ((4 - (l % 4)) % 4);
  }
  return { type: msg.readUInt16BE(0), tid: msg.subarray(8, 20).toString('hex'), attrs };
}

function xorDecode(v) {
  if (!v || v.length < 8 || v[1] !== 1) return null; // IPv4 only
  const ip = [0, 1, 2, 3].map((i) => v[4 + i] ^ ((STUN_COOKIE >>> (24 - i * 8)) & 0xff));
  return { address: ip.join('.'), port: v.readUInt16BE(2) ^ (STUN_COOKIE >>> 16) };
}

function xorEncode({ address, port }) {
  const v = Buffer.alloc(8);
  v[1] = 1;
  v.writeUInt16BE(port ^ (STUN_COOKIE >>> 16), 2);
  address.split('.').forEach((x, i) => { v[4 + i] = Number(x) ^ ((STUN_COOKIE >>> (24 - i * 8)) & 0xff); });
  return v;
}

const stunError = (m) => {
  const v = m.attrs.get(ATTR.ERROR_CODE);
  return v && v.length >= 4 ? (v[2] & 7) * 100 + v[3] : 0;
};

/** One allocation on the TURN server; the other app is reached through channels (4-byte header). */
class TurnClient {
  constructor(ep, { address, port, username, credential }) {
    this.ep = ep;
    this.server = { address, port };
    this.username = String(username);
    this.password = String(credential);
    this.realm = null;
    this.nonce = null;
    this.key = null;
    this.pending = new Map();
    this.channels = new Map(); // 'ip:port' → { number, peer, ready }
    this.byNumber = new Map();
    this.nextChannel = 0x4000;
    this.relayed = null;
    this.mapped = null;
    this.refreshTimer = null;
    this.closed = false;
  }

  transaction(tid, msg) {
    return new Promise((resolve) => {
      const key = tid.toString('hex');
      let tries = 0;
      const entry = {
        timer: null,
        resolve: (m) => {
          clearTimeout(entry.timer);
          this.pending.delete(key);
          resolve(m);
        },
      };
      const send = () => {
        if (this.closed || tries++ >= 4) {
          entry.resolve(null);
          return;
        }
        this.ep.socket?.send(msg, this.server.port, this.server.address);
        entry.timer = setTimeout(send, 250 * tries);
      };
      this.pending.set(key, entry);
      send();
    });
  }

  async request(method, attrs) {
    for (let attempt = 0; attempt < 4; attempt++) {
      const auth = this.realm ? [[ATTR.USERNAME, Buffer.from(this.username)], [ATTR.REALM, Buffer.from(this.realm)], [ATTR.NONCE, Buffer.from(this.nonce)]] : [];
      const tid = crypto.randomBytes(12);
      const answer = await this.transaction(tid, stunEncode(method, tid, [...attrs, ...auth], this.realm ? this.key : null));
      if (!answer) throw new Error('TURN server does not answer');
      if ((answer.type & 0x0110) === 0x0100) return answer;
      const code = stunError(answer);
      if (code === 401 || code === 438) {
        // (new) realm and nonce: authenticate with the long-term credential
        const realm = answer.attrs.get(ATTR.REALM);
        const nonce = answer.attrs.get(ATTR.NONCE);
        if (realm) this.realm = realm.toString();
        if (nonce) this.nonce = nonce.toString();
        this.key = crypto.createHash('md5').update(`${this.username}:${this.realm}:${this.password}`).digest();
        continue;
      }
      throw new Error(`TURN error ${code}`);
    }
    throw new Error('TURN authentication failed');
  }

  async allocate() {
    const res = await this.request(METHOD.ALLOCATE, [[ATTR.REQUESTED_TRANSPORT, Buffer.from([17, 0, 0, 0])], [ATTR.LIFETIME, u32(600)]]);
    this.relayed = xorDecode(res.attrs.get(ATTR.XOR_RELAYED_ADDRESS));
    this.mapped = xorDecode(res.attrs.get(ATTR.XOR_MAPPED_ADDRESS));
    if (!this.relayed) throw new Error('TURN: no relayed address');
    // allocations live 10 minutes, channels 10, permissions 5: refresh well before
    this.refreshTimer = setInterval(() => {
      this.request(METHOD.REFRESH, [[ATTR.LIFETIME, u32(600)]]).catch(() => {});
      for (const ch of this.channels.values()) if (ch.ready) this.channelBind(ch).catch(() => {});
    }, 120000);
    return this;
  }

  channelBind(ch) {
    const n = Buffer.alloc(4);
    n.writeUInt16BE(ch.number, 0);
    return this.request(METHOD.CHANNEL_BIND, [[ATTR.CHANNEL_NUMBER, n], [ATTR.XOR_PEER_ADDRESS, xorEncode(ch.peer)]]);
  }

  /** A channel to the other app's relayed address (also opens the permission for it). */
  async bind(peer) {
    const key = `${peer.address}:${peer.port}`;
    let ch = this.channels.get(key);
    if (!ch) {
      if (this.nextChannel > 0x7ffe) return;
      ch = { number: this.nextChannel++, peer: { address: peer.address, port: peer.port }, ready: false };
      this.channels.set(key, ch);
      this.byNumber.set(ch.number, ch);
    }
    if (!ch.ready) {
      await this.channelBind(ch);
      ch.ready = true;
    }
  }

  bound(address, port) {
    return Boolean(this.channels.get(`${address}:${port}`)?.ready);
  }

  wrap(address, port, packet) {
    const ch = this.channels.get(`${address}:${port}`);
    if (!ch?.ready) return null;
    const out = Buffer.allocUnsafe(4 + packet.length);
    out.writeUInt16BE(ch.number, 0);
    out.writeUInt16BE(packet.length, 2);
    packet.copy(out, 4);
    return out;
  }

  /** A packet from the TURN server: { peer, data } for the other app's packets, else null. */
  onMessage(msg) {
    if (msg.length >= 4 && msg[0] >= 0x40 && msg[0] <= 0x7f) {
      const ch = this.byNumber.get(msg.readUInt16BE(0));
      const len = msg.readUInt16BE(2);
      return ch && 4 + len <= msg.length ? { peer: ch.peer, data: msg.subarray(4, 4 + len) } : null;
    }
    const m = stunParse(msg);
    if (!m) return null;
    if (m.type === DATA_INDICATION) {
      const peer = xorDecode(m.attrs.get(ATTR.XOR_PEER_ADDRESS));
      const data = m.attrs.get(ATTR.DATA);
      return peer && data ? { peer, data } : null;
    }
    this.pending.get(m.tid)?.resolve(m);
    return null;
  }

  close() {
    if (this.closed) return;
    clearInterval(this.refreshTimer);
    if (this.relayed && this.key) {
      // give the allocation back (sent once, no waiting)
      const tid = crypto.randomBytes(12);
      const auth = [[ATTR.USERNAME, Buffer.from(this.username)], [ATTR.REALM, Buffer.from(this.realm)], [ATTR.NONCE, Buffer.from(this.nonce)]];
      try {
        this.ep.socket?.send(stunEncode(METHOD.REFRESH, tid, [[ATTR.LIFETIME, u32(0)], ...auth], this.key), this.server.port, this.server.address);
      } catch {
        // socket already gone
      }
    }
    this.closed = true;
    for (const p of [...this.pending.values()]) p.resolve(null);
  }
}

/* ------------------------------------------------ one connection */

class Peer {
  constructor(ep, id, session, secret) {
    this.ep = ep;
    this.id = id;
    this.session = session >>> 0;
    this.secret = secret;
    this.kp = keyPair();
    this.keys = null;
    this.counter = 0n;
    this.state = 'new';
    this.path = null; // { address, port, kind: 'lan' | 'internet' | 'relay', rtt }
    this.cands = [];
    this.probes = new Map();
    this.probeStart = 0;
    this.probeUntil = 0;
    this.lastRecv = 0;
    this.lastSent = 0;
    this.publicAddr = null;
    this.regTimer = null;
    // sending (pacing)
    this.out = [];
    this.outHead = 0;
    this.outBytes = 0;
    this.tokens = 0;
    this.lastFill = now();
    this.sentBytes = 0;
    // receiving video (viewer)
    this.expect = null;
    this.missing = new Map();
    this.frames = new Map();
    this.next = null;
    this.minNext = 0;
    this.waitSince = 0;
    this.lastPli = 0;
    this.nackPending = false;
    this.stats = { packets: 0, lost: 0, recovered: 0, nacks: 0, skips: 0, bytes: 0, frames: 0 };
    this.clock = { rtt: null, offset: null, samples: [] };
    this.remoteReport = null;
  }

  get sendKey() { return this.ep.role === 'host' ? this.keys.toViewer : this.keys.toHost; }
  get recvKey() { return this.ep.role === 'host' ? this.keys.toHost : this.keys.toViewer; }
  get sendDir() { return this.ep.role === 'host' ? 1 : 2; }
  get recvDir() { return this.ep.role === 'host' ? 2 : 1; }

  packet(type, payload) {
    this.counter += 1n;
    return seal(this.sendKey, this.sendDir, type, this.session, this.counter, payload);
  }

  sendTo(address, port, type, payload) {
    if (!this.keys) return;
    this.ep.transmit(this.packet(type, payload), address, port);
  }

  send(type, payload) {
    if (!this.path) return;
    this.lastSent = now();
    this.sendTo(this.path.address, this.path.port, type, payload);
  }
}

/* ------------------------------------------------ the endpoint (one per app window) */

class DirectEndpoint extends EventEmitter {
  /**
   * role: 'host' (sends the picture) or 'viewer'
   * serverHost: the signaling server's host name; its UDP port comes with each session
   */
  constructor({ role, serverHost, forceRelay = false, forceTurn = false }) {
    super();
    this.role = role;
    this.serverHost = serverHost;
    this.forceRelay = forceRelay; // tests / troubleshooting: only the server's relay …
    this.forceTurn = forceTurn; // … or only the TURN server
    this.turn = null;
    this.turnPromise = null;
    this.socket = null;
    this.port = 0;
    this.server = null; // { address, port }
    this.peers = new Map();
    this.bySession = new Map();
    this.videoSeq = 0;
    this.videoFrame = 0;
    this.audioSeq = 0;
    this.ring = new Array(RING);
    this.ringSeq = new Uint32Array(RING);
    this.paceRate = (200e6 / 8) / 1000; // bytes per ms until the page tells the bitrate
    this.draining = false;
    this.tickTimer = null;
    this.closed = false;
  }

  async open() {
    const socket = dgram.createSocket({ type: 'udp4' });
    socket.on('message', (msg, rinfo) => {
      try {
        this.onMessage(msg, rinfo);
      } catch (err) {
        this.emit('log', `direct: ${err?.stack || err}`);
      }
    });
    socket.on('error', (err) => this.emit('log', `direct socket: ${err.message}`));
    await new Promise((resolve, reject) => {
      socket.once('error', reject);
      socket.bind(0, () => {
        socket.off('error', reject);
        resolve();
      });
    });
    // big frames arrive as bursts: room for a 4K keyframe or two
    for (const [fn, size] of [['setRecvBufferSize', 8 << 20], ['setSendBufferSize', 4 << 20]]) {
      try {
        socket[fn](size);
      } catch {
        // the system keeps its own limit
      }
    }
    this.socket = socket;
    this.port = socket.address().port;
    this.tickTimer = setInterval(() => this.tick(), 5);
    return this.port;
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    for (const id of [...this.peers.keys()]) this.removePeer(id);
    clearInterval(this.tickTimer);
    this.turn?.close();
    try {
      this.socket?.close();
    } catch {
      // already closed
    }
    this.socket = null;
    this.removeAllListeners();
  }

  async resolveServer(port) {
    if (this.server && this.server.port === port) return this.server;
    const { address } = await dns.promises.lookup(this.serverHost, { family: 4 });
    this.server = { address, port };
    return this.server;
  }

  /**
   * Starts a connection; resolves to what the other side needs: { pub, cands }.
   * port: the server's UDP port for direct mode; turn: { host, port, username, credential }.
   */
  async addPeer(id, { session, secret, port, turn }) {
    this.removePeer(id);
    const peer = new Peer(this, id, Number(session), Buffer.from(String(secret), 'base64'));
    this.peers.set(id, peer);
    this.bySession.set(peer.session, peer);
    await Promise.all([
      port && !this.forceTurn ? this.resolveServer(Number(port)).then(() => this.register(peer)).catch((err) => {
        this.emit('log', `direct: server not reachable over UDP (${err.message})`);
      }) : null,
      turn && !this.forceRelay ? this.ensureTurn(turn) : null,
    ]);
    if (this.peers.get(id) !== peer) throw new Error('closed');
    const forced = this.forceRelay || this.forceTurn;
    const cands = forced ? [] : localAddresses().map((address) => ({ address, port: this.port }));
    // the public address: from the server's UDP port, or from the TURN server
    const pub = peer.publicAddr || this.turn?.mapped;
    if (pub && !forced) cands.push({ address: pub.address, port: pub.port });
    if (this.turn?.relayed) cands.push({ ...this.turn.relayed, turn: true });
    return { pub: peer.kp.pub, cands: cands.slice(0, MAX_CANDS) };
  }

  /** One TURN allocation per window, shared by all connections; null when it cannot be had. */
  ensureTurn(info) {
    if (!this.turnPromise) {
      this.turnPromise = (async () => {
        const { address } = await dns.promises.lookup(String(info.host), { family: 4 });
        const turn = new TurnClient(this, { address, port: Number(info.port) || 3478, username: info.username, credential: info.credential });
        this.turn = turn; // answers are routed to it from now on
        await turn.allocate();
        return turn;
      })().catch((err) => {
        this.emit('log', `direct: no TURN allocation (${err.message})`);
        this.turn?.close();
        this.turn = null;
        this.turnPromise = null; // the next connection tries again
        return null;
      });
    }
    return this.turnPromise;
  }

  /** Sends to an address – through the TURN server when it is the other app's relayed address. */
  transmit(packet, address, port) {
    if (!this.socket) return;
    const wrapped = this.turn?.wrap(address, port, packet);
    if (wrapped) this.socket.send(wrapped, this.turn.server.port, this.turn.server.address);
    else this.socket.send(packet, port, address);
  }

  /** Registers at the server (public address, relay); repeated so the NAT mapping stays open. */
  register(peer) {
    return new Promise((resolve) => {
      let tries = 0;
      const send = () => {
        if (this.peers.get(peer.id) !== peer || !this.server) return;
        this.socket.send(registration(this.role, peer.session, peer.secret), this.server.port, this.server.address);
      };
      peer.onRegistered = (addr) => {
        peer.publicAddr = addr;
        clearInterval(timer);
        resolve(addr);
        clearInterval(peer.regTimer);
        peer.regTimer = setInterval(send, 15000);
      };
      const timer = setInterval(() => {
        if (++tries > 10 || this.peers.get(peer.id) !== peer) {
          clearInterval(timer);
          resolve(null);
        } else {
          send();
        }
      }, 250);
      send();
    });
  }

  /** The other side's answer arrived: derive the keys and find a path. */
  connectPeer(id, { pub, cands }) {
    const peer = this.peers.get(id);
    if (!peer) return;
    peer.keys = deriveKeys(peer.kp.privateKey, pub, peer.session);
    peer.cands = (Array.isArray(cands) ? cands : []).filter(validCandidate).slice(0, MAX_CANDS).map((c) => ({ address: c.address, port: Number(c.port), turn: c.turn === true }));
    // the other app's TURN address is reached through our own allocation
    for (const c of peer.cands) {
      if (c.turn && this.turn) this.turn.bind(c).catch((err) => this.emit('log', `direct: TURN channel (${err.message})`));
    }
    peer.state = 'connecting';
    this.startProbing(peer, 10000);
    peer.failTimer = setTimeout(() => {
      if (peer.state !== 'connecting' || this.peers.get(id) !== peer) return;
      // what could be reached, for the page to explain
      this.setState(peer, 'failed', { server: Boolean(peer.publicAddr), turn: Boolean(this.turn?.relayed), remote: peer.cands.length });
    }, 7000);
  }

  removePeer(id) {
    const peer = this.peers.get(id);
    if (!peer) return;
    if (peer.path) peer.send(T.BYE, Buffer.alloc(1));
    clearInterval(peer.regTimer);
    clearTimeout(peer.failTimer);
    this.peers.delete(id);
    if (this.bySession.get(peer.session) === peer) this.bySession.delete(peer.session);
  }

  setState(peer, state, diag = null) {
    peer.state = state;
    this.emit('state', peer.id, { state, path: peer.path?.kind || null, rtt: peer.path?.rtt ?? null, ...(diag ? { diag } : {}) });
  }

  /* ---- finding a path */

  startProbing(peer, ms) {
    peer.probeStart = now();
    peer.probeUntil = peer.probeStart + ms;
    peer.nextProbe = 0;
  }

  probe(peer, t) {
    if (t < peer.nextProbe || t > peer.probeUntil || !peer.keys) return;
    peer.nextProbe = t + 100;
    const since = t - peer.probeStart;
    const targets = this.forceRelay || this.forceTurn ? [] : peer.cands.filter((c) => !c.turn);
    // the relays only after a moment: a direct path is better
    if (this.server && !this.forceTurn && (this.forceRelay || since > 1000)) targets.push({ ...this.server, relay: true });
    if (this.turn && !this.forceRelay && (this.forceTurn || since > 1500)) targets.push(...peer.cands.filter((c) => c.turn && this.turn.bound(c.address, c.port)));
    for (const target of targets) {
      const id = crypto.randomBytes(8);
      peer.probes.set(id.toString('hex'), { sent: t, relay: Boolean(target.relay) });
      peer.sendTo(target.address, target.port, T.PROBE, id);
    }
    if (peer.probes.size > 400) peer.probes.clear();
  }

  onProbeAck(peer, body, rinfo) {
    const probe = peer.probes.get(body.subarray(0, 8).toString('hex'));
    if (!probe) return;
    const t = now();
    const relay = Boolean(this.server && rinfo.address === this.server.address && rinfo.port === this.server.port);
    const kind = rinfo.turn ? 'turn' : relay ? 'relay' : isPrivate(rinfo.address) ? 'lan' : 'internet';
    const candidate = { address: rinfo.address, port: rinfo.port, kind, rtt: t - probe.sent };
    const rank = (p) => ({ lan: 0, internet: 1, relay: 2, turn: 3 })[p.kind];
    const current = peer.path;
    // take it if there is none yet, or it is a better kind, or the same kind and clearly faster
    const better = !current || rank(candidate) < rank(current) || (rank(candidate) === rank(current) && candidate.rtt + 2 < current.rtt && current.address !== candidate.address);
    if (better) {
      peer.path = candidate;
      if (peer.state !== 'connected') {
        clearTimeout(peer.failTimer);
        this.setState(peer, 'connected');
      } else {
        this.emit('state', peer.id, { state: 'connected', path: kind, rtt: candidate.rtt });
      }
    } else if (current && current.address === candidate.address && current.port === candidate.port) {
      current.rtt = candidate.rtt;
    }
  }

  /* ---- receiving */

  onMessage(msg, rinfo) {
    if (msg.length < 1) return;
    if (this.turn && rinfo.address === this.turn.server.address && rinfo.port === this.turn.server.port) {
      // through the TURN server: the other app's packet, as if it came from its relayed address
      const inner = this.turn.onMessage(msg);
      if (inner && inner.data[0] === PEER) this.onMessage(inner.data, { address: inner.peer.address, port: inner.peer.port, turn: true });
      return;
    }
    if (msg[0] === REG_ACK) {
      const ack = parseRegistrationAck(msg, (s) => this.bySession.get(s)?.secret);
      const peer = ack && this.bySession.get(ack.session);
      if (peer && this.server && rinfo.address === this.server.address) peer.onRegistered?.({ address: ack.address, port: ack.port });
      return;
    }
    if (msg[0] !== PEER || msg.length < HEADER + TAG) return;
    const peer = this.bySession.get(msg.readUInt32BE(2));
    if (!peer || !peer.keys) return;
    const body = unseal(peer.recvKey, peer.recvDir, msg);
    if (!body) return;
    const t = now();
    peer.lastRecv = t;
    peer.stats.bytes += msg.length;
    switch (msg[1]) {
      case T.PROBE:
        peer.sendTo(rinfo.address, rinfo.port, T.PROBE_ACK, body.subarray(0, 8));
        break;
      case T.PROBE_ACK:
        this.onProbeAck(peer, body, rinfo);
        break;
      case T.MEDIA:
        if (this.role === 'viewer') this.onMedia(peer, body, t);
        break;
      case T.NACK:
        if (this.role === 'host') this.onNack(peer, body);
        break;
      case T.PLI:
        if (this.role === 'host') this.emit('keyframe', peer.id);
        break;
      case T.PING:
        if (body.length >= 8) {
          const pong = Buffer.alloc(16);
          body.copy(pong, 0, 0, 8);
          pong.writeDoubleBE(t, 8);
          peer.send(T.PONG, pong);
        }
        break;
      case T.PONG:
        if (body.length >= 16) this.onPong(peer, body.readDoubleBE(0), body.readDoubleBE(8), t);
        break;
      case T.REPORT:
        try {
          peer.remoteReport = JSON.parse(body.toString('utf8'));
          this.emit('report', peer.id, peer.remoteReport);
        } catch {
          // ignore
        }
        break;
      case T.BYE:
        this.setState(peer, 'closed');
        peer.path = null;
        break;
      default:
        break;
    }
  }

  onPong(peer, t1, t2, t3) {
    const rtt = t3 - t1;
    if (!(rtt >= 0 && rtt < 10000)) return;
    const c = peer.clock;
    c.samples.push({ rtt, offset: t2 - (t1 + t3) / 2 });
    if (c.samples.length > 20) c.samples.shift();
    const best = c.samples.reduce((a, b) => (b.rtt < a.rtt ? b : a));
    c.rtt = rtt;
    c.offset = best.offset; // the least delayed sample says the most about the clocks
    if (peer.path) peer.path.rtt = rtt;
    this.emit('clock', peer.id, { rtt, offset: c.offset });
  }

  onMedia(peer, body, t) {
    if (body.length < MEDIA_HEADER) return;
    const stream = body[0];
    const flags = body[1];
    const seq = body.readUInt32BE(4);
    const frameId = body.readUInt32BE(8);
    const index = body.readUInt16BE(12);
    const count = body.readUInt16BE(14);
    const capture = body.readDoubleBE(16);
    const ts = body.readDoubleBE(24);
    const data = body.subarray(MEDIA_HEADER);
    peer.stats.packets++;
    if (stream === 1) {
      // audio: one packet per Opus frame, played as it comes (late audio is useless)
      if (count === 1) this.emit('media', { stream: 1, sub: body[2], ts, capture, key: true }, data);
      return;
    }
    this.trackSequence(peer, seq, t);
    if (peer.next != null && ((frameId - peer.next) | 0) < 0) return; // already delivered or skipped
    let f = peer.frames.get(frameId);
    if (!f) {
      if (count < 1 || peer.frames.size > 600) return;
      f = { id: frameId, count, parts: new Array(count), got: 0, bytes: 0, key: Boolean(flags & 1), ts, capture, first: t };
      peer.frames.set(frameId, f);
    }
    if (count !== f.count || index >= count || f.parts[index]) return;
    f.parts[index] = data;
    f.got++;
    f.bytes += data.length;
    if (f.got === f.count) this.deliver(peer, t);
  }

  trackSequence(peer, seq, t) {
    if (peer.expect == null) {
      peer.expect = (seq + 1) >>> 0;
      return;
    }
    const d = (seq - peer.expect) | 0;
    if (d === 0) {
      peer.expect = (seq + 1) >>> 0;
    } else if (d > 0) {
      if (d < 4000) {
        for (let s = peer.expect; s !== seq; s = (s + 1) >>> 0) peer.missing.set(s, { first: t, last: 0, tries: 0 });
        peer.stats.lost += d;
        this.scheduleNack(peer);
      }
      peer.expect = (seq + 1) >>> 0;
    } else if (peer.missing.delete(seq)) {
      peer.stats.recovered++;
    }
  }

  // How long a frame with lost packets waits for the retransmission. Skipping costs more (a
  // keyframe has to come), so not too eager – a busy computer can be late by a few ms too.
  giveUpMs(peer) {
    const rtt = peer.clock.rtt ?? peer.path?.rtt ?? 30;
    return Math.min(300, Math.max(60, rtt * 3 + 30));
  }

  /** Hands complete frames to the page in order; skips to the next keyframe when one cannot be completed. */
  deliver(peer, t) {
    for (;;) {
      if (peer.next == null) {
        let key = null;
        for (const f of peer.frames.values()) {
          if (f.key && f.got === f.count && ((f.id - peer.minNext) | 0) >= 0 && (!key || ((f.id - key.id) | 0) < 0)) key = f;
        }
        if (!key) {
          this.requestKeyframe(peer, t);
          // without a keyframe, the frames after it are of no use
          for (const [id, f] of peer.frames) if (!f.key && t - f.first > 1000) peer.frames.delete(id);
          return;
        }
        peer.next = key.id;
        for (const id of [...peer.frames.keys()]) if (((id - key.id) | 0) < 0) peer.frames.delete(id);
      }
      const f = peer.frames.get(peer.next);
      if (f && f.got === f.count) {
        peer.frames.delete(f.id);
        peer.next = (peer.next + 1) >>> 0;
        peer.waitSince = 0;
        peer.stats.frames++;
        this.emit('media', { stream: 0, key: f.key, ts: f.ts, capture: f.capture, id: f.id }, f.parts.length === 1 ? f.parts[0] : Buffer.concat(f.parts, f.bytes));
        continue;
      }
      // A later frame is here but this one is not complete: packets were lost. Wait for the
      // retransmission, but not long. (Without a later frame it is simply still arriving –
      // a big keyframe takes a while.)
      const later = [...peer.frames.keys()].some((id) => ((id - peer.next) | 0) > 0);
      if (!later) return;
      if (!peer.waitSince) peer.waitSince = t;
      if (t - peer.waitSince < this.giveUpMs(peer)) return;
      peer.stats.skips++;
      peer.minNext = (peer.next + 1) >>> 0;
      peer.next = null;
      peer.waitSince = 0;
    }
  }

  /** The page could not decode: ask the sharer for a keyframe. */
  keyframe(id) {
    this.requestKeyframe(this.peers.get(id));
  }

  requestKeyframe(peer, t = now()) {
    if (!peer?.path || t - peer.lastPli < 150) return;
    peer.lastPli = t;
    peer.send(T.PLI, Buffer.alloc(1));
  }

  scheduleNack(peer) {
    if (peer.nackPending) return;
    peer.nackPending = true;
    setImmediate(() => {
      peer.nackPending = false;
      this.sendNacks(peer, now());
    });
  }

  sendNacks(peer, t) {
    if (!peer.missing.size || !peer.path) return;
    const rtt = peer.clock.rtt ?? peer.path.rtt ?? 30;
    const resend = Math.max(8, rtt * 1.5);
    const seqs = [];
    for (const [seq, m] of peer.missing) {
      if (t - m.first > 1000 || (m.tries >= 4 && t - m.last > resend)) {
        peer.missing.delete(seq);
        continue;
      }
      if (m.tries < 4 && (m.last === 0 || t - m.last > resend)) {
        m.last = t;
        m.tries++;
        seqs.push(seq);
      }
    }
    for (let i = 0; i < seqs.length; i += 280) {
      const chunk = seqs.slice(i, i + 280);
      const body = Buffer.alloc(chunk.length * 4);
      chunk.forEach((s, j) => body.writeUInt32BE(s, j * 4));
      peer.send(T.NACK, body);
      peer.stats.nacks += chunk.length;
    }
  }

  /* ---- sending (host) */

  setBitrate(bps) {
    // a frame leaves at 3× the video bitrate: fast, but not one big burst into the network
    const pace = Math.max(3 * Number(bps || 0), 40e6);
    this.paceRate = pace / 8 / 1000;
  }

  /** One encoded frame (video) or Opus packet (audio) to every connected viewer. */
  sendMedia(meta, data) {
    if (this.role !== 'host') return;
    const buf = Buffer.isBuffer(data) ? data : Buffer.from(data.buffer, data.byteOffset, data.byteLength);
    const video = meta.stream === 0;
    const count = Math.max(1, Math.ceil(buf.length / MAX_DATA));
    if (count > 65535) return;
    const frameId = video ? this.videoFrame : this.audioSeq;
    if (video) this.videoFrame = (this.videoFrame + 1) >>> 0;
    const ready = [...this.peers.values()].filter((p) => p.state === 'connected' && p.path);
    for (let i = 0; i < count; i++) {
      const part = buf.subarray(i * MAX_DATA, Math.min(buf.length, (i + 1) * MAX_DATA));
      const p = Buffer.allocUnsafe(MEDIA_HEADER + part.length);
      p[0] = video ? 0 : 1;
      p[1] = meta.key ? 1 : 0;
      p[2] = Number(meta.sub) & 0xff;
      p[3] = 0;
      const seq = video ? this.videoSeq : this.audioSeq;
      if (video) this.videoSeq = (this.videoSeq + 1) >>> 0;
      else this.audioSeq = (this.audioSeq + 1) >>> 0;
      p.writeUInt32BE(seq, 4);
      p.writeUInt32BE(frameId, 8);
      p.writeUInt16BE(i, 12);
      p.writeUInt16BE(count, 14);
      p.writeDoubleBE(Number(meta.capture) || 0, 16);
      p.writeDoubleBE(Number(meta.ts) || 0, 24);
      part.copy(p, MEDIA_HEADER);
      if (video) {
        this.ring[seq & (RING - 1)] = p;
        this.ringSeq[seq & (RING - 1)] = seq;
      }
      for (const peer of ready) this.enqueue(peer, p, !video);
    }
    this.drain();
  }

  enqueue(peer, payload, urgent = false) {
    if (urgent) {
      // audio and retransmissions jump the queue
      peer.send(T.MEDIA, payload);
      return;
    }
    if (peer.outBytes > 8 << 20) {
      // the viewer's path cannot keep up: start over from the next keyframe
      peer.out = [];
      peer.outHead = 0;
      peer.outBytes = 0;
      this.emit('keyframe', peer.id);
      return;
    }
    peer.out.push(payload);
    peer.outBytes += payload.length;
  }

  drain() {
    const t = now();
    let pending = false;
    for (const peer of this.peers.values()) {
      peer.tokens = Math.min(64 * 1024, peer.tokens + (t - peer.lastFill) * this.paceRate);
      peer.lastFill = t;
      while (peer.outHead < peer.out.length && peer.tokens > 0) {
        const p = peer.out[peer.outHead++];
        peer.outBytes -= p.length;
        peer.tokens -= p.length + HEADER + TAG + 28;
        peer.sentBytes += p.length;
        peer.send(T.MEDIA, p);
      }
      if (peer.outHead >= peer.out.length) {
        peer.out = [];
        peer.outHead = 0;
      } else {
        pending = true;
      }
    }
    if (pending && !this.draining) {
      this.draining = true;
      setImmediate(() => {
        this.draining = false;
        this.drain();
      });
    }
  }

  onNack(peer, body) {
    for (let i = 0; i + 4 <= body.length && i < 4 * 1000; i += 4) {
      const seq = body.readUInt32BE(i);
      const p = this.ring[seq & (RING - 1)];
      if (!p || this.ringSeq[seq & (RING - 1)] !== seq) continue;
      const again = Buffer.from(p);
      again[1] |= 2; // retransmission
      this.enqueue(peer, again, true);
    }
  }

  /* ---- timers */

  tick() {
    const t = now();
    for (const peer of this.peers.values()) {
      if (peer.keys) this.probe(peer, t);
      if (peer.state !== 'connected') continue;
      // the path went quiet: look for one again (the relay included)
      if (t - peer.lastRecv > 2000 && t > peer.probeUntil) this.startProbing(peer, 8000);
      if (t - peer.lastRecv > 6000) {
        peer.path = null;
        this.setState(peer, 'lost');
        continue;
      }
      if (this.role === 'viewer') {
        if (t - (peer.lastPing || 0) >= 500) {
          peer.lastPing = t;
          const ping = Buffer.alloc(8);
          ping.writeDoubleBE(t, 0);
          peer.send(T.PING, ping);
        }
        if (peer.missing.size) this.sendNacks(peer, t);
        if (peer.next == null || peer.waitSince || peer.frames.size) this.deliver(peer, t);
        if (t - (peer.lastReport || 0) >= 1000) this.report(peer, t);
      }
    }
  }

  report(peer, t) {
    const s = peer.stats;
    const prev = peer.prevStats || { packets: 0, lost: 0, recovered: 0, bytes: 0, frames: 0, skips: 0, at: t - 1000 };
    const packets = s.packets - prev.packets;
    const lost = Math.max(0, s.lost - prev.lost - (s.recovered - prev.recovered));
    const out = {
      loss: packets + lost > 0 ? Math.round((lost / (packets + lost)) * 1000) / 10 : 0,
      kbps: Math.round(((s.bytes - prev.bytes) * 8) / Math.max(1, t - prev.at)),
      fps: Math.round(((s.frames - prev.frames) * 1000) / Math.max(1, t - prev.at)),
      skips: s.skips - prev.skips,
      rtt: peer.clock.rtt != null ? Math.round(peer.clock.rtt * 10) / 10 : null,
      path: peer.path?.kind || null,
    };
    peer.prevStats = { ...s, at: t };
    peer.lastReport = t;
    peer.send(T.REPORT, Buffer.from(JSON.stringify(out)));
    this.emit('stats', peer.id, out);
  }
}

module.exports = {
  DirectEndpoint,
  MAX_DATA,
  _test: { keyPair, deriveKeys, seal, unseal, registration, parseRegistrationAck, isPrivate, validCandidate, T, stunEncode, stunParse, xorEncode, xorDecode },
};
