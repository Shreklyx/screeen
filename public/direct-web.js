'use strict';

// Direct mode in the browser: the apps' own protocol (native/core/direct.cpp) instead of
// WebRTC, so a browser can watch an app – or share to apps and browsers – the same way.
//
// A browser cannot send UDP, so its packets travel in a WebSocket to the server
// (wss://…/direct), which passes them to the other side (the app's "tcp" path). The packets
// are encrypted end to end like the app's (X25519 + HKDF → ChaCha20-Poly1305, or AES-128-GCM with an older
// app – our own code: the apps' C++ as WebAssembly, ss-web.js): the server pairs the two ends by session
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
  // and what comes after it needs it) – beyond that they are dropped, not delayed. Only a line clearly
  // slower than the picture: every drop costs that viewer a keyframe.
  const QUEUE_MS = 250;
  const KEY_QUEUE_MS = 1000;
  const INFO = new TextEncoder().encode('screenshare-direct-v1');
  const INFO2 = new TextEncoder().encode('screenshare-direct-v2'); // (the room's own key mixed in)
  // The cipher. A browser has no AES instructions for WebAssembly, and ChaCha20-Poly1305 (RFC 8439) is
  // several times faster in software – our own code too; the apps keep AES-128-GCM between themselves
  // (their CPUs' AES instructions). ChaCha20-Poly1305 when both sides have it and one prefers it; an older
  // app (no list): AES-128-GCM. Its keys: 32 bytes each way, from their own label.
  const AES = 'aes-128-gcm';
  const CHACHA = 'chacha20-poly1305';
  const AEADS = [CHACHA, AES];
  const INFO_CHACHA = new TextEncoder().encode('screenshare-direct-v1-chacha20');
  const INFO2_CHACHA = new TextEncoder().encode('screenshare-direct-v2-chacha20');
  const useChaCha = (theirs) => Array.isArray(theirs) && theirs.includes(CHACHA); // (ours lists it first)
  const now = () => performance.timeOrigin + performance.now();

  const b64 = (bytes) => btoa(String.fromCharCode(...bytes));
  const unb64 = (s) => Uint8Array.from(atob(String(s)), (c) => c.charCodeAt(0));
  const randomBytes = (n) => crypto.getRandomValues(new Uint8Array(n));
  const hex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');

  /* ------------------------------------------------ our own cryptography (ss-web.wasm: the apps' C++) */

  // our WebAssembly (ss-web.js): the cryptography and the lossless sound – null when it cannot be loaded
  let wasm;
  const loadWasm = async () => {
    if (wasm === undefined) wasm = await (globalThis.ScreenShareWasm?.load() ?? Promise.reject(new Error('no ss-web.js'))).catch(() => null);
    return wasm;
  };
  const BASE_POINT = new Uint8Array(32);
  BASE_POINT[0] = 9;

  /** A key pair for the exchange (X25519, RFC 7748): { pub, shared(theirPub) → 32 bytes, forget() }. */
  function newKeys(c) {
    const secret = randomBytes(32); // (the system's random numbers, as the apps')
    return {
      pub: c.x25519(secret, BASE_POINT),
      shared(theirs) {
        const s = c.x25519(secret, theirs);
        if (!s) throw new Error('a low-order public key');
        return s;
      },
      forget() {
        secret.fill(0);
      },
    };
  }

  let support = null;
  /** What this browser can do: { ok } – a WebSocket, WebAssembly (our cryptography) and an H.264 decoder,
   *  at least; audio: 'wasm' (the lossless sound: our FLAC decoder and an AudioWorklet) | null. */
  function supported() {
    support ||= (async () => {
      const no = { ok: false };
      if (typeof WebSocket !== 'function' || typeof VideoDecoder !== 'function' || typeof WebAssembly !== 'object') return no;
      try {
        if ((await VideoDecoder.isConfigSupported({ codec: 'avc1.640034' })).supported !== true) return no;
      } catch {
        return no;
      }
      if (!(await loadWasm())) return no;
      const audio = typeof AudioWorkletNode === 'function' ? 'wasm' : null;
      return { ok: true, keys: 'ours', audio };
    })();
    return support;
  }

  /* ------------------------------------------------ one connection */

  class Peer {
    constructor(id, session, role) {
      this.id = id;
      this.session = session >>> 0;
      this.role = role;
      this.kx = null; // our key pair (see newKeys)
      this.hmacKey = null; // the session's secret (the server's HMAC key for this connection)
      this.sendKey = null; // one key each way (ss-web.js chacha() or gcm())
      this.recvKey = null;
      this.aead = null; // which: CHACHA or AES
      this.counter = 0n;
      this.state = 'new';
      this.path = null; // { kind: 'tcp', rtt }
      this.ws = null;
      this.ready = false;
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
      this.keyAsked = -Infinity; // when a keyframe was last asked for this viewer (while frames are dropped)
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

    /** Encrypts (our AES-GCM: at once, no promise to wait for) and sends. */
    send(type, payload) {
      if (!this.sendKey || !this.ready || this.ws?.readyState !== WebSocket.OPEN) return;
      this.counter += 1n;
      const packet = new Uint8Array(HEADER + payload.length + 16);
      const view = new DataView(packet.buffer);
      packet[0] = PEER;
      packet[1] = type;
      view.setUint32(2, this.session);
      view.setBigUint64(6, this.counter);
      const iv = new Uint8Array(12);
      iv[0] = this.sendDir;
      iv.set(packet.subarray(6, 14), 4);
      this.sendKey.seal(iv, packet.subarray(0, HEADER), payload, packet, HEADER);
      this.ws.send(packet);
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
      const w = await loadWasm();
      if (!w) throw new Error('no ss-web.wasm');
      if (this.peers.get(id) !== peer) throw new Error('closed');
      peer.kx = newKeys(w.crypto);
      peer.hmacKey = unb64(secret);
      this.openSocket(peer);
      // no UDP addresses: the other side uses the WebSocket path at once
      return { pub: b64(peer.kx.pub), cands: [], tcp: true, aead: AEADS };
    }

    /** The WebSocket to the server; the first message proves the session (HMAC of its secret). */
    openSocket(peer) {
      const ws = new WebSocket(this.tcpUrl);
      ws.binaryType = 'arraybuffer';
      peer.ws = ws;
      ws.onopen = () => {
        const role = ROLE[this.role];
        const nonce = hex(randomBytes(8));
        const mac = wasm.crypto.hmacSha256(peer.hmacKey, `tcp:${peer.session}:${role}:${nonce}`).subarray(0, 16);
        ws.send(JSON.stringify({ session: peer.session, role, nonce, mac: b64(mac) }));
      };
      ws.onmessage = (e) => {
        if (typeof e.data === 'string') {
          if (e.data === 'ok') peer.ready = true;
          return;
        }
        const packet = new Uint8Array(e.data);
        const body = this.open(peer, packet); // (at once, in the order they came)
        if (body) this.onPacket(peer, packet, body);
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
    async connectPeer(id, { pub, psk, aead }) {
      const peer = this.peers.get(id);
      if (!peer) return;
      try {
        const c = wasm.crypto;
        const shared = peer.kx.shared(unb64(pub));
        // the room's own key (when both sides proved they hold it) goes into the keys too
        const withKey = psk?.length === 16;
        const ikm = withKey ? new Uint8Array(48) : shared;
        if (withKey) {
          ikm.set(shared);
          ikm.set(psk, 32);
        }
        const salt = new Uint8Array(4);
        new DataView(salt.buffer).setUint32(0, peer.session);
        const chacha = useChaCha(aead);
        const n = chacha ? 32 : 16;
        const okm = c.hkdfSha256(salt, ikm, chacha ? (withKey ? INFO2_CHACHA : INFO_CHACHA) : withKey ? INFO2 : INFO, 2 * n);
        ikm.fill(0);
        shared.fill(0);
        const key = (raw) => (chacha ? c.chacha(raw) : c.gcm(raw));
        const toViewer = key(okm.subarray(0, n));
        const toHost = key(okm.subarray(n, 2 * n));
        okm.fill(0);
        peer.sendKey?.close();
        peer.recvKey?.close();
        peer.sendKey = this.role === 'host' ? toViewer : toHost;
        peer.recvKey = this.role === 'host' ? toHost : toViewer;
        peer.aead = chacha ? CHACHA : AES;
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
      this.peers.delete(id);
      const ws = peer.ws;
      peer.ws = null;
      try {
        ws?.close(); // (after the goodbye: what was sent goes first)
      } catch {
        // closed
      }
      peer.sendKey?.close(); // (the keys gone from memory)
      peer.recvKey?.close();
      peer.sendKey = peer.recvKey = null;
      peer.kx?.forget();
    }

    setState(peer, state, diag = null) {
      peer.state = state;
      this.emit('state', peer.id, { state, path: peer.path?.kind || null, rtt: peer.path?.rtt ?? null, aead: peer.aead, ...(diag ? { diag } : {}) });
    }

    /* ---- receiving */

    /** A packet's body, decrypted (our AES-GCM) – or null (not ours, or it does not open). */
    open(peer, packet) {
      if (packet.length < HEADER + 16 || packet[0] !== PEER || !peer.recvKey) return null;
      const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
      if (view.getUint32(2) !== peer.session) return null;
      const iv = new Uint8Array(12);
      iv[0] = peer.recvDir;
      iv.set(packet.subarray(6, 14), 4);
      return peer.recvKey.open(iv, packet.subarray(0, HEADER), packet.subarray(HEADER));
    }

    onPacket(peer, packet, body) {
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

    /** One encoded frame or sound packet (FLAC) to every connected viewer – whole, at once. */
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
          // (frames never confirmed for 5 s are forgotten)
          while (peer.inFlight.length && t - peer.inFlight[0].at > 5000) peer.inFlight.shift();
          let late = false;
          if (peer.acks && peer.inFlight.length) {
            const base = Math.min(...peer.ackMin);
            const oldest = peer.inFlight[0];
            late = t - oldest.at - (Number.isFinite(base) ? base : 0) > (oldest.key ? KEY_QUEUE_MS : QUEUE_MS);
          }
          const full = queued > limit || late;
          if (full || (peer.dropping && !meta.key)) {
            // dropped: the viewer's picture goes on only from a keyframe – asked for as the dropping starts,
            // and again once there is room (the one asked for may have come while there was none, and the
            // viewer, getting nothing, asks for nothing: the picture would stand still, the sound going on)
            if (!peer.dropping || (!full && t - peer.keyAsked > 300)) {
              peer.keyAsked = t;
              this.emit('keyframe', peer.id);
            }
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
