// Direct mode (desktop app ↔ desktop app, no WebRTC): the server's UDP port.
//
//  - Registration: an app sends 'R' + session + HMAC (the session secret came over the
//    WebSocket); the answer tells it the public address the server saw – the address the
//    other app can try to reach it on.
//  - Relay: when the two apps cannot reach each other directly, their (end-to-end
//    encrypted) packets come here and are forwarded between the two registered addresses.
//    Nothing is ever sent to an address that has not registered with the session secret.
//  - TCP fallback (acceptTcp): when no UDP gets through at all (strict firewalls), the two apps
//    send the same encrypted packets in a WebSocket (wss://…/direct) and this passes them on.

import dgram from 'node:dgram';
import crypto from 'node:crypto';

const REG = 0x52;
const REG_ACK = 0x41;
const PEER = 0x53;
const ROLES = [1, 2]; // sharer, viewer
const MAX_SESSIONS = 500;
const END_TTL_MS = 90 * 1000; // a registration (or relayed packet) keeps an end alive this long

const hmac16 = (secret, data) => crypto.createHmac('sha256', secret).update(data).digest().subarray(0, 16);

export function createRelay({ port = 0, host } = {}) {
  const sessions = new Map(); // id → { secret, room, viewer, ends: { 1: end, 2: end } }
  const socket = dgram.createSocket({ type: 'udp4' });
  let sweepTimer = null;

  function create({ room, viewer }) {
    if (sessions.size >= MAX_SESSIONS) {
      // the oldest one goes (a viewer that never connected)
      sessions.delete(sessions.keys().next().value);
    }
    let id;
    do id = crypto.randomBytes(4).readUInt32BE(0); while (!id || sessions.has(id));
    const secret = crypto.randomBytes(16);
    sessions.set(id, { secret, room, viewer, ends: {}, created: Date.now(), relayed: 0 });
    return { session: id, secret: secret.toString('base64') };
  }

  function removeWhere(match) {
    for (const [id, s] of sessions) {
      if (!match(s)) continue;
      sessions.delete(id);
      for (const ws of Object.values(s.tcp || {})) ws.close();
    }
  }

  /** A WebSocket from an app: the first (text) message proves the session, then binary packets pass through. */
  function acceptTcp(ws) {
    const timer = setTimeout(() => ws.terminate(), 10000);
    ws.once('message', (data, isBinary) => {
      clearTimeout(timer);
      let m;
      try {
        m = isBinary ? null : JSON.parse(data.toString());
      } catch {
        m = null;
      }
      const s = m && sessions.get(Number(m.session));
      const role = m && Number(m.role);
      const mac = s && ROLES.includes(role) && typeof m.nonce === 'string' && typeof m.mac === 'string'
        ? Buffer.from(m.mac, 'base64') : null;
      if (!mac || mac.length !== 16 || !crypto.timingSafeEqual(hmac16(s.secret, Buffer.from(`tcp:${Number(m.session)}:${role}:${m.nonce}`)), mac)) {
        ws.close(1008, 'auth');
        return;
      }
      s.tcp ||= {};
      if (s.tcp[role] && s.tcp[role] !== ws) s.tcp[role].close();
      s.tcp[role] = ws;
      ws.on('message', (packet, binary) => {
        const other = s.tcp?.[role === 1 ? 2 : 1];
        // late packets are worth nothing: a peer that does not keep up loses them (NACK / keyframe)
        if (!binary || !other || other.readyState !== other.OPEN || other.bufferedAmount > 8 << 20) return;
        s.relayed += packet.length;
        other.send(packet, { binary: true });
      });
      ws.on('close', () => { if (s.tcp?.[role] === ws) delete s.tcp[role]; });
      ws.send('ok');
    });
    ws.on('error', () => {});
  }

  function onRegister(msg, rinfo) {
    if (msg.length < 30 || !ROLES.includes(msg[1])) return;
    const s = sessions.get(msg.readUInt32BE(2));
    if (!s || !crypto.timingSafeEqual(hmac16(s.secret, msg.subarray(0, 14)), msg.subarray(14, 30))) return;
    s.ends[msg[1]] = { address: rinfo.address, port: rinfo.port, seen: Date.now() };
    const ip = rinfo.address.split('.').map(Number);
    if (ip.length !== 4) return;
    const ack = Buffer.alloc(37);
    ack[0] = REG_ACK;
    ack[1] = msg[1];
    msg.copy(ack, 2, 2, 14); // session + the client's nonce
    ack[14] = 4;
    ip.forEach((b, i) => { ack[15 + i] = b; });
    ack.writeUInt16BE(rinfo.port, 19);
    hmac16(s.secret, ack.subarray(0, 21)).copy(ack, 21);
    socket.send(ack, rinfo.port, rinfo.address);
  }

  function onPeerPacket(msg, rinfo) {
    if (msg.length < 30) return;
    const s = sessions.get(msg.readUInt32BE(2));
    if (!s) return;
    const now = Date.now();
    for (const role of ROLES) {
      const from = s.ends[role];
      if (!from || from.address !== rinfo.address || from.port !== rinfo.port) continue;
      const to = s.ends[role === 1 ? 2 : 1];
      from.seen = now;
      if (!to || now - to.seen > END_TTL_MS) return;
      s.relayed += msg.length;
      socket.send(msg, to.port, to.address);
      return;
    }
  }

  socket.on('message', (msg, rinfo) => {
    try {
      if (msg[0] === REG) onRegister(msg, rinfo);
      else if (msg[0] === PEER) onPeerPacket(msg, rinfo);
    } catch (err) {
      console.error('[screenshare] relay error', err);
    }
  });
  socket.on('error', (err) => console.error('[screenshare] relay socket', err.message));

  return {
    sessions,
    create,
    removeWhere,
    acceptTcp,
    listen() {
      return new Promise((resolve, reject) => {
        socket.once('error', reject);
        socket.bind(port, host, () => {
          socket.off('error', reject);
          // 4K bursts from several sharers at once
          try { socket.setRecvBufferSize(8 << 20); } catch { /* system limit */ }
          try { socket.setSendBufferSize(8 << 20); } catch { /* system limit */ }
          sweepTimer = setInterval(() => {
            const now = Date.now();
            for (const [id, s] of sessions) {
              for (const role of ROLES) if (s.ends[role] && now - s.ends[role].seen > END_TTL_MS) delete s.ends[role];
              if (now - s.created > 24 * 3600 * 1000) sessions.delete(id);
            }
          }, 30000);
          sweepTimer.unref();
          resolve(socket.address().port);
        });
      });
    },
    close() {
      clearInterval(sweepTimer);
      try { socket.close(); } catch { /* not bound */ }
    },
  };
}
