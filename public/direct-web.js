'use strict';

// Direct mode in the browser: the desktop app's own protocol (desktop/direct.js) instead of
// WebRTC, so a browser can watch an app – or share to apps and browsers – the same way.
//
// A browser cannot send UDP, so its packets travel in a WebSocket to the server
// (wss://…/direct), which passes them to the other side (the app's "tcp" path). The packets
// are encrypted end to end like the app's (X25519 + HKDF → AES-128-GCM, WebCrypto – X25519 from
// our WebAssembly, ss-web.js, where WebCrypto has none): the server pairs the two ends by session
// and HMAC, but cannot read the picture.
// Over the WebSocket a frame goes as one packet (TCP loses nothing, so no NACK); a frame that
// cannot be sent in time is dropped instead of waiting in a queue, and the viewer asks for a
// keyframe. The viewer confirms each frame (ACK): a queue anywhere on the way – the server, its
// proxy, the sockets – shows at the sharer, which drops frames before the delay grows. The viewer
// hands every frame to the page the moment it arrives.
//
// window.ScreenShareWebDirect: { supported(): Promise<{ ok, keys, audio }>, create({ tcpUrl }) } – the
// object create() returns has the same calls as the apps' engine.

(() => {
  const PEER = 0x53;
  const T = { PROBE: 1, PROBE_ACK: 2, MEDIA: 3, NACK: 4, PLI: 5, PING: 6, PONG: 7, REPORT: 8, BYE: 9, ACK: 11 };
  const ROLE = { host: 1, viewer: 2 };
  const HEADER = 14;
  const MEDIA_HEADER = 32;
  const WHOLE = 4; // media flag: the whole frame in this one packet
  // how long frames may queue on their way to a viewer that confirms them (a keyframe longer: it is big,
  // and what comes after it needs it) – beyond that they are dropped, not delayed
  const QUEUE_MS = 120;
  const KEY_QUEUE_MS = 400;
  const INFO = new TextEncoder().encode('screenshare-direct-v1');
  const subtle = globalThis.crypto?.subtle;
  const now = () => performance.timeOrigin + performance.now();

  const b64 = (bytes) => btoa(String.fromCharCode(...bytes));
  const unb64 = (s) => Uint8Array.from(atob(String(s)), (c) => c.charCodeAt(0));
  const randomBytes = (n) => crypto.getRandomValues(new Uint8Array(n));
  const hex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');

  /* ------------------------------------------------ the key exchange: WebCrypto, or our WebAssembly */

  // generate() → { pub, shared(theirPub) → Promise<32 bytes>, wipe?() }
  const webCryptoKeys = {
    async generate() {
      const kp = await subtle.generateKey({ name: 'X25519' }, true, ['deriveBits']);
      return {
        pub: new Uint8Array(await subtle.exportKey('raw', kp.publicKey)),
        async shared(theirs) {
          const other = await subtle.importKey('raw', theirs, { name: 'X25519' }, false, []);
          return new Uint8Array(await subtle.deriveBits({ name: 'X25519', public: other }, kp.privateKey, 256));
        },
      };
    },
  };
  const wasmKeys = (wasm) => ({
    async generate() {
      const secret = randomBytes(32);
      return {
        pub: wasm.x25519(secret),
        async shared(theirs) {
          return wasm.x25519(secret, theirs);
        },
        wipe() {
          secret.fill(0);
        },
      };
    },
  });
  // our WebAssembly (ss-web.js), loaded when needed; null when it cannot be
  let wasm;
  const loadWasm = async () => {
    if (wasm === undefined) wasm = await (globalThis.ScreenShareWasm?.load() ?? Promise.reject(new Error('no ss-web.js'))).catch(() => null);
    return wasm;
  };

  let keys = null;
  /** The key exchange: WebCrypto's X25519 where the browser has it, else ours. */
  async function keyExchange() {
    if (keys) return keys;
    try {
      await subtle.generateKey({ name: 'X25519' }, false, ['deriveBits']);
      keys = webCryptoKeys;
    } catch {
      if (!(await loadWasm())) throw new Error('no key exchange');
      keys = wasmKeys(wasm);
    }
    return keys;
  }

  let support = null;
  /** What this browser can do: { ok } – a WebSocket, WebCrypto and an H.264 decoder, at least; keys:
   *  'webcrypto' | 'wasm' (X25519); audio: 'webcodecs' | 'wasm' (an Opus decoder) | null. */
  function supported() {
    support ||= (async () => {
      const no = { ok: false };
      if (!subtle || typeof WebSocket !== 'function' || typeof VideoDecoder !== 'function') return no;
      try {
        if ((await VideoDecoder.isConfigSupported({ codec: 'avc1.640034' })).supported !== true) return no;
      } catch {
        return no;
      }
      try {
        await keyExchange();
      } catch {
        return no;
      }
      let audio = null;
      try {
        if (typeof AudioDecoder === 'function' && (await AudioDecoder.isConfigSupported({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 })).supported === true) audio = 'webcodecs';
      } catch {
        // our decoder then
      }
      if (!audio && typeof AudioWorkletNode === 'function' && (await loadWasm())) audio = 'wasm';
      return { ok: true, keys: keys === webCryptoKeys ? 'webcrypto' : 'wasm', audio };
    })();
    return support;
  }

  /* ------------------------------------------------ one connection */

  class Peer {
    constructor(id, session, role) {
      this.id = id;
      this.session = session >>> 0;
      this.role = role;
      this.kx = null; // our key pair (see webCryptoKeys)
      this.hmacKey = null;
      this.sendKey = null;
      this.recvKey = null;
      this.counter = 0n;
      this.state = 'new';
      this.path = null; // { kind: 'tcp', rtt }
      this.ws = null;
      this.ready = false;
      this.sendChain = Promise.resolve();
      this.recvChain = Promise.resolve();
      this.probes = new Map();
      this.probeUntil = 0;
      this.nextProbe = 0;
      this.lastRecv = 0;
      this.failTimer = null;
      // sending
      this.seq = 0;
      this.dropping = false;
      this.bps = 0;
      // a viewer that confirms its frames (ACK): those still on their way – a queue anywhere between
      // (the server, its proxy, the sockets) shows as frames confirmed late
      this.inFlight = []; // { id, at, key }
      this.acks = false;
      this.ackMin = [Infinity, Infinity]; // the least time a frame took to be confirmed, in two 5 s windows
      this.ackWindowAt = 0;
      // receiving video
      this.frames = new Map();
      this.next = null;
      this.minNext = 0;
      this.lastPli = 0;
      this.stats = { packets: 0, bytes: 0, frames: 0, skips: 0 };
      this.prevStats = null;
      this.clock = { rtt: null, offset: null, samples: [] };
    }

    get sendDir() { return this.role === 'host' ? 1 : 2; }
    get recvDir() { return this.role === 'host' ? 2 : 1; }

    /** Encrypts and sends, in order (WebCrypto answers asynchronously). */
    send(type, payload) {
      if (!this.sendKey || !this.ready) return;
      this.counter += 1n;
      const counter = this.counter;
      this.sendChain = this.sendChain.then(async () => {
        const head = new Uint8Array(HEADER);
        const view = new DataView(head.buffer);
        head[0] = PEER;
        head[1] = type;
        view.setUint32(2, this.session);
        view.setBigUint64(6, counter);
        const iv = new Uint8Array(12);
        iv[0] = this.sendDir;
        iv.set(head.subarray(6, 14), 4);
        const sealed = new Uint8Array(await subtle.encrypt({ name: 'AES-GCM', iv, additionalData: head }, this.sendKey, payload));
        const packet = new Uint8Array(HEADER + sealed.length);
        packet.set(head);
        packet.set(sealed, HEADER);
        if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(packet);
      }).catch(() => {});
    }
  }

  /* ------------------------------------------------ the endpoint (one per page) */

  class Endpoint {
    constructor({ role, tcpUrl, emit }) {
      this.role = role;
      this.tcpUrl = tcpUrl;
      this.emit = emit;
      this.peers = new Map();
      this.videoFrame = 0;
      this.audioSeq = 0;
      this.bps = 0;
      this.timer = setInterval(() => this.tick(), 20);
    }

    close() {
      for (const id of [...this.peers.keys()]) this.removePeer(id);
      clearInterval(this.timer);
    }

    async addPeer(id, { session, secret }) {
      this.removePeer(id);
      const peer = new Peer(id, Number(session), this.role);
      this.peers.set(id, peer);
      peer.kx = await (await keyExchange()).generate();
      peer.hmacKey = await subtle.importKey('raw', unb64(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
      if (this.peers.get(id) !== peer) throw new Error('closed');
      this.openSocket(peer);
      // no UDP addresses: the other side uses the WebSocket path at once
      return { pub: b64(peer.kx.pub), cands: [], tcp: true };
    }

    /** The WebSocket to the server; the first message proves the session (HMAC of its secret). */
    openSocket(peer) {
      const ws = new WebSocket(this.tcpUrl);
      ws.binaryType = 'arraybuffer';
      peer.ws = ws;
      ws.onopen = async () => {
        const role = ROLE[this.role];
        const nonce = hex(randomBytes(8));
        const mac = new Uint8Array(await subtle.sign('HMAC', peer.hmacKey, new TextEncoder().encode(`tcp:${peer.session}:${role}:${nonce}`))).subarray(0, 16);
        if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ session: peer.session, role, nonce, mac: b64(mac) }));
      };
      ws.onmessage = (e) => {
        if (typeof e.data === 'string') {
          if (e.data === 'ok') peer.ready = true;
          return;
        }
        const packet = new Uint8Array(e.data);
        peer.recvChain = peer.recvChain.then(() => this.onPacket(peer, packet)).catch(() => {});
      };
      ws.onclose = () => {
        if (peer.ws !== ws) return;
        peer.ws = null;
        peer.ready = false;
        if (this.peers.get(peer.id) !== peer) return;
        if (peer.state === 'connected') {
          peer.path = null;
          this.setState(peer, 'lost');
        } else if (peer.state === 'connecting') {
          clearTimeout(peer.failTimer);
          this.setState(peer, 'failed', { server: false, turn: false, tcp: false, remote: 0 });
        }
      };
      ws.onerror = () => {};
    }

    /** The other side's answer: the keys, then probes until it answers. */
    async connectPeer(id, { pub }) {
      const peer = this.peers.get(id);
      if (!peer) return;
      try {
        const shared = await peer.kx.shared(unb64(pub));
        const hkdf = await subtle.importKey('raw', shared, 'HKDF', false, ['deriveBits']);
        const salt = new Uint8Array(4);
        new DataView(salt.buffer).setUint32(0, peer.session);
        const okm = new Uint8Array(await subtle.deriveBits({ name: 'HKDF', hash: 'SHA-256', salt, info: INFO }, hkdf, 256));
        const aes = (raw) => subtle.importKey('raw', raw, 'AES-GCM', false, ['encrypt', 'decrypt']);
        const [toViewer, toHost] = await Promise.all([aes(okm.subarray(0, 16)), aes(okm.subarray(16, 32))]);
        if (this.peers.get(id) !== peer) return;
        peer.sendKey = this.role === 'host' ? toViewer : toHost;
        peer.recvKey = this.role === 'host' ? toHost : toViewer;
      } catch {
        this.setState(peer, 'failed', { server: false, turn: false, tcp: peer.ready, remote: 0 });
        return;
      }
      peer.state = 'connecting';
      peer.probeUntil = now() + 10000;
      peer.failTimer = setTimeout(() => {
        if (peer.state === 'connecting' && this.peers.get(id) === peer) this.setState(peer, 'failed', { server: false, turn: false, tcp: peer.ready, remote: 0 });
      }, 8000);
    }

    removePeer(id) {
      const peer = this.peers.get(id);
      if (!peer) return;
      if (peer.path) peer.send(T.BYE, new Uint8Array(1));
      clearTimeout(peer.failTimer);
      peer.kx?.wipe?.(); // (our key pair: the private half does not stay in memory)
      this.peers.delete(id);
      const ws = peer.ws;
      peer.ws = null;
      // after the goodbye has gone
      peer.sendChain.then(() => { try { ws?.close(); } catch { /* closed */ } });
    }

    setState(peer, state, diag = null) {
      peer.state = state;
      this.emit('state', peer.id, { state, path: peer.path?.kind || null, rtt: peer.path?.rtt ?? null, ...(diag ? { diag } : {}) });
    }

    /* ---- receiving */

    async onPacket(peer, packet) {
      if (packet.length < HEADER + 16 || packet[0] !== PEER || !peer.recvKey) return;
      const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
      if (view.getUint32(2) !== peer.session) return;
      const iv = new Uint8Array(12);
      iv[0] = peer.recvDir;
      iv.set(packet.subarray(6, 14), 4);
      let body;
      try {
        body = new Uint8Array(await subtle.decrypt({ name: 'AES-GCM', iv, additionalData: packet.subarray(0, HEADER) }, peer.recvKey, packet.subarray(HEADER)));
      } catch {
        return;
      }
      if (this.peers.get(peer.id) !== peer) return;
      const t = now();
      peer.lastRecv = t;
      peer.stats.bytes += packet.length;
      const bv = new DataView(body.buffer, body.byteOffset, body.byteLength);
      switch (packet[1]) {
        case T.PROBE:
          peer.send(T.PROBE_ACK, body.slice(0, 8));
          break;
        case T.PROBE_ACK: {
          const probe = peer.probes.get(hex(body.subarray(0, 8)));
          if (!probe) break;
          const rtt = t - probe;
          if (!peer.path) {
            peer.path = { kind: 'tcp', rtt };
            clearTimeout(peer.failTimer);
            this.setState(peer, 'connected');
          } else {
            peer.path.rtt = rtt;
          }
          break;
        }
        case T.MEDIA:
          if (this.role === 'viewer') this.onMedia(peer, body, bv, t);
          break;
        case T.PLI:
          if (this.role === 'host') this.emit('keyframe', peer.id);
          break;
        case T.ACK:
          if (this.role === 'host' && body.length >= 4) this.onAck(peer, bv.getUint32(0), t);
          break;
        case T.PING:
          if (body.length >= 8) {
            const pong = new Uint8Array(16);
            pong.set(body.subarray(0, 8));
            new DataView(pong.buffer).setFloat64(8, t);
            peer.send(T.PONG, pong);
          }
          break;
        case T.PONG:
          if (body.length >= 16) this.onPong(peer, bv.getFloat64(0), bv.getFloat64(8), t);
          break;
        case T.REPORT:
          try {
            this.emit('report', peer.id, JSON.parse(new TextDecoder().decode(body)));
          } catch {
            // ignore
          }
          break;
        case T.BYE:
          peer.path = null;
          this.setState(peer, 'closed');
          break;
        default:
          break; // NACK: over the WebSocket nothing is lost on the way
      }
    }

    onPong(peer, t1, t2, t3) {
      const rtt = t3 - t1;
      if (!(rtt >= 0 && rtt < 10000)) return;
      const c = peer.clock;
      c.samples.push({ rtt, offset: t2 - (t1 + t3) / 2 });
      if (c.samples.length > 20) c.samples.shift();
      c.rtt = rtt;
      c.offset = c.samples.reduce((a, b) => (b.rtt < a.rtt ? b : a)).offset;
      if (peer.path) peer.path.rtt = rtt;
      this.emit('clock', peer.id, { rtt, offset: c.offset });
    }

    onMedia(peer, body, bv, t) {
      if (body.length < MEDIA_HEADER) return;
      const stream = body[0];
      const frameId = bv.getUint32(8);
      const index = bv.getUint16(12);
      const count = bv.getUint16(14);
      const capture = bv.getFloat64(16);
      const ts = bv.getFloat64(24);
      const data = body.subarray(MEDIA_HEADER);
      peer.stats.packets++;
      if (stream === 1) {
        if (count === 1) this.emit('media', { stream: 1, sub: body[2], ts, capture, key: true }, data);
        return;
      }
      // the sharer learns how far the frames have come (see onAck)
      const ack = new Uint8Array(4);
      new DataView(ack.buffer).setUint32(0, frameId);
      peer.send(T.ACK, ack);
      if (peer.next != null && ((frameId - peer.next) | 0) < 0) return;
      let f = peer.frames.get(frameId);
      if (!f) {
        if (count < 1 || peer.frames.size > 600) return;
        f = { id: frameId, count, parts: new Array(count), got: 0, bytes: 0, key: Boolean(body[1] & 1), ts, capture, first: t };
        peer.frames.set(frameId, f);
      }
      if (count !== f.count || index >= count || f.parts[index]) return;
      f.parts[index] = data;
      f.got++;
      f.bytes += data.length;
      this.deliver(peer, t);
    }

    /** Complete frames to the page, in order. In a TCP stream nothing comes late: a frame missing
     *  when a later one is here was dropped – skip it and continue from a keyframe. */
    deliver(peer, t) {
      for (;;) {
        if (peer.next == null) {
          let key = null;
          for (const f of peer.frames.values()) {
            if (f.key && f.got === f.count && ((f.id - peer.minNext) | 0) >= 0 && (!key || ((f.id - key.id) | 0) < 0)) key = f;
          }
          if (!key) {
            this.requestKeyframe(peer, t);
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
          peer.stats.frames++;
          let data = f.parts[0];
          if (f.count > 1) {
            data = new Uint8Array(f.bytes);
            let at = 0;
            for (const p of f.parts) {
              data.set(p, at);
              at += p.length;
            }
          }
          this.emit('media', { stream: 0, key: f.key, ts: f.ts, capture: f.capture, id: f.id }, data);
          continue;
        }
        if (![...peer.frames.keys()].some((id) => ((id - peer.next) | 0) > 0)) return;
        peer.stats.skips++;
        peer.minNext = (peer.next + 1) >>> 0;
        peer.next = null;
      }
    }

    /** Host: the viewer has this frame – and every one before it. */
    onAck(peer, id, t) {
      peer.acks = true;
      while (peer.inFlight.length && ((peer.inFlight[0].id - id) | 0) <= 0) {
        const f = peer.inFlight.shift();
        if (f.id !== id) continue;
        if (t - peer.ackWindowAt > 5000) {
          peer.ackMin = [Infinity, peer.ackMin[0]];
          peer.ackWindowAt = t;
        }
        peer.ackMin[0] = Math.min(peer.ackMin[0], t - f.at);
      }
    }

    requestKeyframe(peer, t = now()) {
      if (!peer?.path || t - peer.lastPli < 150) return;
      peer.lastPli = t;
      peer.send(T.PLI, new Uint8Array(1));
    }

    /* ---- sending (host) */

    /** One encoded frame or Opus packet to every connected viewer – whole, at once. */
    sendMedia(meta, data) {
      if (this.role !== 'host') return;
      const video = meta.stream === 0;
      const frameId = video ? this.videoFrame : this.audioSeq;
      if (video) this.videoFrame = (this.videoFrame + 1) >>> 0;
      else this.audioSeq = (this.audioSeq + 1) >>> 0;
      // what may wait in the socket: ~150 ms of video; more is only delay
      const limit = Math.max(256 * 1024, (this.bps / 8) * 0.15);
      const t = now();
      for (const peer of this.peers.values()) {
        if (peer.state !== 'connected' || !peer.ws) continue;
        const queued = peer.ws.bufferedAmount;
        if (video) {
          // a viewer that confirms its frames: how long the oldest one still on its way has waited beyond
          // the path's own time – what queues between here and the viewer, where this socket does not see it
          let late = false;
          if (peer.acks && peer.inFlight.length) {
            const base = Math.min(...peer.ackMin);
            const oldest = peer.inFlight[0];
            late = t - oldest.at - (Number.isFinite(base) ? base : 0) > (oldest.key ? KEY_QUEUE_MS : QUEUE_MS);
          }
          if (queued > limit || late || (peer.dropping && !meta.key)) {
            if (!peer.dropping) this.emit('keyframe', peer.id); // the next one goes when there is room
            peer.dropping = true;
            continue;
          }
          peer.dropping = false;
        } else if (queued > limit * 4) {
          continue;
        }
        const p = new Uint8Array(MEDIA_HEADER + data.byteLength);
        const v = new DataView(p.buffer);
        p[0] = video ? 0 : 1;
        p[1] = (meta.key ? 1 : 0) | WHOLE;
        p[2] = Number(meta.sub) & 0xff;
        v.setUint32(4, peer.seq);
        peer.seq = (peer.seq + 1) >>> 0;
        v.setUint32(8, frameId);
        v.setUint16(12, 0);
        v.setUint16(14, 1);
        v.setFloat64(16, Number(meta.capture) || 0);
        v.setFloat64(24, Number(meta.ts) || 0);
        p.set(data instanceof Uint8Array ? data : new Uint8Array(data), MEDIA_HEADER);
        peer.send(T.MEDIA, p);
        if (video) {
          peer.inFlight.push({ id: frameId, at: t, key: Boolean(meta.key) });
          if (peer.inFlight.length > 600) peer.inFlight.shift(); // (a viewer that confirms nothing any more)
        }
      }
    }

    /* ---- timers */

    tick() {
      const t = now();
      for (const peer of this.peers.values()) {
        if (peer.state === 'connecting' && peer.ready && peer.sendKey && t >= peer.nextProbe && t < peer.probeUntil) {
          peer.nextProbe = t + 100;
          const id = randomBytes(8);
          peer.probes.set(hex(id), t);
          if (peer.probes.size > 200) peer.probes.clear();
          peer.send(T.PROBE, id);
        }
        if (peer.state !== 'connected') continue;
        if (t - peer.lastRecv > 6000) {
          peer.path = null;
          this.setState(peer, 'lost');
          continue;
        }
        if (this.role === 'viewer') {
          if (t - (peer.lastPing || 0) >= 500) {
            peer.lastPing = t;
            const ping = new Uint8Array(8);
            new DataView(ping.buffer).setFloat64(0, t);
            peer.send(T.PING, ping);
          }
          if (t - (peer.lastReport || 0) >= 1000) this.report(peer, t);
        }
      }
    }

    report(peer, t) {
      const s = peer.stats;
      const prev = peer.prevStats || { packets: 0, bytes: 0, frames: 0, skips: 0, at: t - 1000 };
      const out = {
        loss: 0,
        kbps: Math.round(((s.bytes - prev.bytes) * 8) / Math.max(1, t - prev.at)),
        fps: Math.round(((s.frames - prev.frames) * 1000) / Math.max(1, t - prev.at)),
        skips: s.skips - prev.skips,
        rtt: peer.clock.rtt != null ? Math.round(peer.clock.rtt * 10) / 10 : null,
        path: 'tcp',
      };
      peer.prevStats = { ...s, at: t };
      peer.lastReport = t;
      peer.send(T.REPORT, new TextEncoder().encode(JSON.stringify(out)));
      this.emit('stats', peer.id, out);
    }
  }

  /* ------------------------------------------------ the page's side (as preload.js `direct`) */

  function create({ tcpUrl }) {
    let ep = null;
    let onEvent = () => {};
    let onMedia = () => {};
    const emit = (name, ...args) => {
      if (name === 'media') onMedia(...args);
      else onEvent(name, ...args);
    };
    const open = (role) => {
      if (ep?.role === role) return ep;
      ep?.close();
      ep = new Endpoint({ role, tcpUrl, emit });
      return ep;
    };
    return {
      web: true,
      add: (role, id, info) => open(role).addPeer(String(id), info).catch((err) => ({ error: err.message })),
      connect: (id, answer) => { ep?.connectPeer(String(id), answer || {}); },
      remove: (id) => ep?.removePeer(String(id)),
      send: (meta, data) => ep?.sendMedia(meta, data),
      keyframe: () => ep?.requestKeyframe(ep.peers.get('host')),
      bitrate: (bps) => { if (ep) ep.bps = Number(bps) || 0; },
      stop: () => {
        ep?.close();
        ep = null;
      },
      onEvent: (cb) => { onEvent = cb; },
      onMedia: (cb) => { onMedia = cb; },
    };
  }

  window.ScreenShareWebDirect = { supported, create };
})();
