// Direct mode transport (desktop/direct.js) against the server's UDP relay (server/direct-relay.js).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import crypto from 'node:crypto';
import { createRelay } from '../server/direct-relay.js';

const require = createRequire(import.meta.url);
const { DirectEndpoint, MAX_DATA, _test: t } = require('../desktop/direct.js');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (cond, ms = 5000) => {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) throw new Error('timeout');
    await sleep(10);
  }
};

test('both sides derive the same keys; packets only open with the right key and direction', () => {
  const a = t.keyPair();
  const b = t.keyPair();
  const ka = t.deriveKeys(a.privateKey, b.pub, 1234);
  const kb = t.deriveKeys(b.privateKey, a.pub, 1234);
  assert.deepEqual(ka, kb);
  assert.notDeepEqual(ka.toViewer, ka.toHost);
  const payload = Buffer.from('hello');
  const p = t.seal(ka.toViewer, 1, t.T.MEDIA, 1234, 7n, payload);
  assert.deepEqual(t.unseal(kb.toViewer, 1, p), payload);
  assert.equal(t.unseal(kb.toViewer, 2, p), null, 'wrong direction');
  assert.equal(t.unseal(kb.toHost, 1, p), null, 'wrong key');
  const tampered = Buffer.from(p);
  tampered[2] ^= 1; // session id is authenticated
  assert.equal(t.unseal(kb.toViewer, 1, tampered), null);
  assert.throws(() => t.deriveKeys(a.privateKey, 'AAAA', 1));
});

test('candidate validation and private ranges', () => {
  assert.ok(t.validCandidate({ address: '192.168.1.5', port: 5000 }));
  assert.ok(!t.validCandidate({ address: '999.1.1.1', port: 5000 }));
  assert.ok(!t.validCandidate({ address: '224.0.0.1', port: 5000 }), 'multicast');
  assert.ok(!t.validCandidate({ address: '0.0.0.0', port: 5000 }));
  assert.ok(!t.validCandidate({ address: '10.0.0.1', port: 70000 }));
  assert.ok(!t.validCandidate({ address: 'example.com', port: 1 }));
  assert.ok(t.isPrivate('10.1.2.3') && t.isPrivate('172.20.0.1') && t.isPrivate('192.168.0.9') && t.isPrivate('100.70.0.1'));
  assert.ok(!t.isPrivate('94.156.37.47') && !t.isPrivate('172.32.0.1'));
});

test('relay: registration answers with the seen address, only with the right secret', async () => {
  const relay = createRelay({ port: 0, host: '127.0.0.1' });
  const port = await relay.listen();
  const { session, secret } = relay.create({ room: 'r', viewer: 'v1' });
  const key = Buffer.from(secret, 'base64');
  const { default: dgram } = await import('node:dgram');
  const sock = dgram.createSocket('udp4');
  await new Promise((r) => sock.bind(0, '127.0.0.1', r));
  const answers = [];
  sock.on('message', (m) => answers.push(m));
  sock.send(t.registration('viewer', session, crypto.randomBytes(16)), port, '127.0.0.1'); // wrong secret
  await sleep(100);
  assert.equal(answers.length, 0);
  sock.send(t.registration('viewer', session, key), port, '127.0.0.1');
  await until(() => answers.length === 1);
  const ack = t.parseRegistrationAck(answers[0], (s) => (s === session ? key : null));
  assert.deepEqual(ack, { session, address: '127.0.0.1', port: sock.address().port });
  assert.ok(answers[0].length <= 64, 'answer not larger than the question');
  assert.equal(t.parseRegistrationAck(answers[0], () => crypto.randomBytes(16)), null);
  sock.close();
  relay.close();
});

async function pair({ forceRelay = false, drop = null } = {}) {
  const relay = createRelay({ port: 0, host: '127.0.0.1' });
  const port = await relay.listen();
  const { session, secret } = relay.create({ room: 'r', viewer: 'v1' });
  const host = new DirectEndpoint({ role: 'host', serverHost: '127.0.0.1', forceRelay });
  const viewer = new DirectEndpoint({ role: 'viewer', serverHost: '127.0.0.1', forceRelay });
  await host.open();
  await viewer.open();
  if (drop) {
    // lose some of the sharer's packets on the way
    const send = host.socket.send.bind(host.socket);
    host.socket.send = (msg, ...rest) => (drop(msg) ? undefined : send(msg, ...rest));
  }
  const states = { host: [], viewer: [] };
  host.on('state', (id, s) => states.host.push(s));
  viewer.on('state', (id, s) => states.viewer.push(s));
  const offer = await host.addPeer('v1', { session, secret, port });
  const answer = await viewer.addPeer('host', { session, secret, port });
  assert.ok(answer.cands.length >= (forceRelay ? 0 : 1));
  viewer.connectPeer('host', offer);
  host.connectPeer('v1', answer);
  await until(() => states.host.some((s) => s.state === 'connected') && states.viewer.some((s) => s.state === 'connected'));
  const close = () => {
    host.close();
    viewer.close();
    relay.close();
  };
  return { relay, host, viewer, states, close };
}

function frame(n, size) {
  const b = Buffer.alloc(size);
  b.writeUInt32BE(n, 0);
  for (let i = 4; i < size; i++) b[i] = (n + i) & 0xff;
  return b;
}

async function stream(host, viewer, { frames = 40, size = 30000, keyEvery = 0 } = {}) {
  const got = [];
  viewer.on('media', (meta, data) => {
    if (meta.stream === 0) got.push({ n: data.readUInt32BE(0), key: meta.key, ok: data.equals(frame(data.readUInt32BE(0), data.length)), capture: meta.capture });
  });
  for (let n = 0; n < frames; n++) {
    host.sendMedia({ stream: 0, key: n === 0 || (keyEvery && n % keyEvery === 0), ts: n * 16667, capture: n }, frame(n, size + (n % 7) * 100));
    await sleep(5);
  }
  return got;
}

test('direct path: frames arrive whole and in order', async () => {
  const { host, viewer, states, close } = await pair();
  try {
    assert.ok(['lan', 'internet'].includes(states.viewer.at(-1).path), `path ${states.viewer.at(-1).path}`);
    const got = await stream(host, viewer, { frames: 40, size: 5 * MAX_DATA + 17 });
    await until(() => got.length === 40);
    assert.deepEqual(got.map((g) => g.n), [...Array(40).keys()]);
    assert.ok(got.every((g) => g.ok));
    // audio goes as it is
    const audio = [];
    viewer.on('media', (meta, data) => { if (meta.stream === 1) audio.push([meta.sub, data.toString()]); });
    host.sendMedia({ stream: 1, sub: 2, ts: 5, capture: 1 }, Buffer.from('opus'));
    await until(() => audio.length === 1);
    assert.deepEqual(audio[0], [2, 'opus']);
  } finally {
    close();
  }
});

test('lost packets are sent again (NACK) – no frame is skipped', async () => {
  let n = 0;
  let resending = false;
  // every 9th media packet is lost the first time it is sent
  const { host, viewer, close } = await pair({ drop: (msg) => !resending && msg[0] === 0x53 && msg[1] === t.T.MEDIA && ++n % 9 === 0 && n < 400 });
  const onNack = host.onNack.bind(host);
  host.onNack = (...args) => {
    resending = true;
    try {
      onNack(...args);
    } finally {
      resending = false;
    }
  };
  try {
    const got = await stream(host, viewer, { frames: 30, size: 8 * MAX_DATA });
    await until(() => got.length === 30);
    assert.deepEqual(got.map((g) => g.n), [...Array(30).keys()]);
    assert.ok(got.every((g) => g.ok));
  } finally {
    close();
  }
});

test('a frame that cannot be completed is skipped; the viewer asks for a keyframe and goes on from it', async () => {
  const { host, viewer, close } = await pair();
  // the first packet of frame 5 never arrives, retransmissions included
  const enqueue = host.enqueue.bind(host);
  host.enqueue = (peer, payload, urgent) => {
    if (payload[0] === 0 && payload.readUInt32BE(8) === 5 && payload.readUInt16BE(12) === 0) return;
    enqueue(peer, payload, urgent);
  };
  // (a request before frame 0 arrived – a busy test machine – is not the one this is about)
  let answered = false;
  host.on('keyframe', () => {
    if (answered || host.videoFrame < 8) return;
    answered = true;
    host.sendMedia({ stream: 0, key: true, ts: 0, capture: 0 }, frame(100, 2 * MAX_DATA));
  });
  try {
    const got = await stream(host, viewer, { frames: 15, size: 3 * MAX_DATA });
    await until(() => got.some((g) => g.n === 100));
    const ns = got.map((g) => g.n);
    // 0–4, then nothing until the keyframe (frames sent after it decode again)
    assert.deepEqual(ns.slice(0, 6), [0, 1, 2, 3, 4, 100], `${ns}`);
    assert.ok(!ns.includes(5));
    assert.ok(got.find((g) => g.n === 100).key);
  } finally {
    close();
  }
});

test('relay: works when only the server can be reached', async () => {
  const { host, viewer, states, relay, close } = await pair({ forceRelay: true });
  try {
    assert.equal(states.viewer.at(-1).path, 'relay');
    assert.equal(states.host.at(-1).path, 'relay');
    const got = await stream(host, viewer, { frames: 20, size: 4 * MAX_DATA });
    await until(() => got.length === 20);
    assert.ok(got.every((g) => g.ok));
    const s = [...relay.sessions.values()][0];
    assert.ok(s.relayed > 20 * 4 * MAX_DATA, `relayed ${s.relayed}`);
  } finally {
    close();
  }
});

test('clock: the viewer learns the round trip and the clock offset', async () => {
  const { viewer, close } = await pair();
  try {
    const clocks = [];
    viewer.on('clock', (id, c) => clocks.push(c));
    await until(() => clocks.length >= 2, 3000);
    assert.ok(clocks.at(-1).rtt >= 0 && clocks.at(-1).rtt < 50);
    assert.ok(Math.abs(clocks.at(-1).offset) < 20, `offset ${clocks.at(-1).offset}`);
  } finally {
    close();
  }
});

test('STUN encoding: XOR addresses round-trip, attributes parse back', () => {
  const addr = { address: '94.156.37.47', port: 49152 };
  assert.deepEqual(t.xorDecode(t.xorEncode(addr)), addr);
  const tid = crypto.randomBytes(12);
  const msg = t.stunEncode(0x0003, tid, [[0x0019, Buffer.from([17, 0, 0, 0])], [0x0006, Buffer.from('abc')]], Buffer.alloc(16, 1));
  const m = t.stunParse(msg);
  assert.equal(m.type, 0x0003);
  assert.equal(m.tid, tid.toString('hex'));
  assert.equal(m.attrs.get(0x0006).toString(), 'abc');
  assert.equal(m.attrs.get(0x0008).length, 20, 'message integrity');
  assert.equal(msg.length % 4, 0);
});

// Against a real TURN server (coturn), when it is installed: the path when only the TURN port is open.
const { execFileSync, spawn } = await import('node:child_process');
let turnserver = null;
try { turnserver = execFileSync('which', ['turnserver'], { encoding: 'utf8' }).trim(); } catch { /* not installed */ }

test('TURN: direct mode through the TURN server (coturn) when nothing else gets through', { skip: !turnserver && 'coturn not installed' }, async () => {
  const { turnCredentials } = await import('../server/server.js');
  const dgram = await import('node:dgram');
  const free = dgram.createSocket('udp4');
  await new Promise((r) => free.bind(0, '127.0.0.1', r));
  const turnPort = free.address().port;
  free.close();
  const proc = spawn(turnserver, ['-n', '--no-cli', '--log-file=stdout', `--listening-port=${turnPort}`, '--listening-ip=127.0.0.1', '--relay-ip=127.0.0.1',
    '--min-port=51000', '--max-port=51100', '--use-auth-secret', '--static-auth-secret=s3cret', '--realm=test', '--fingerprint',
    '--no-tls', '--no-dtls', '--no-tcp-relay', '--allow-loopback-peers', `--pidfile=/tmp/turn-${turnPort}.pid`], { stdio: 'ignore' });
  await sleep(1500); // coturn starting up
  const relay = createRelay({ port: 0, host: '127.0.0.1' });
  const port = await relay.listen();
  const { session, secret } = relay.create({ room: 'r', viewer: 'v1' });
  const turnInfo = () => ({ host: '127.0.0.1', port: turnPort, ...turnCredentials('s3cret') });
  const host = new DirectEndpoint({ role: 'host', serverHost: '127.0.0.1', forceTurn: true });
  const viewer = new DirectEndpoint({ role: 'viewer', serverHost: '127.0.0.1', forceTurn: true });
  try {
    await host.open();
    await viewer.open();
    const states = [];
    viewer.on('state', (id, s) => states.push(s));
    const offer = await host.addPeer('v1', { session, secret, port, turn: turnInfo() });
    const answer = await viewer.addPeer('host', { session, secret, port, turn: turnInfo() });
    assert.ok(offer.cands.some((c) => c.turn), 'the sharer offers its TURN address');
    assert.ok(answer.cands.some((c) => c.turn), 'the viewer offers its TURN address');
    viewer.connectPeer('host', offer);
    host.connectPeer('v1', answer);
    await until(() => states.some((s) => s.state === 'connected'), 8000);
    assert.equal(states.at(-1).path, 'turn');
    const got = await stream(host, viewer, { frames: 15, size: 4 * MAX_DATA });
    await until(() => got.length === 15);
    assert.ok(got.every((g) => g.ok));
  } finally {
    host.close();
    viewer.close();
    relay.close();
    proc.kill();
  }
});

test('TCP fallback: through the server\'s WebSocket when no UDP gets through', async () => {
  const { createApp } = await import('../server/server.js');
  const app = createApp({ port: 0, host: '127.0.0.1', hostPassword: 'x', turnSecret: '', directPort: 0 });
  await new Promise((r) => app.server.listen(0, '127.0.0.1', r));
  const tcpUrl = `ws://127.0.0.1:${app.server.address().port}/direct`;
  const { session, secret } = app.relay.create({ room: 'r', viewer: 'v1' });
  const host = new DirectEndpoint({ role: 'host', serverHost: '127.0.0.1', tcpUrl, forceTcp: true });
  const viewer = new DirectEndpoint({ role: 'viewer', serverHost: '127.0.0.1', tcpUrl, forceTcp: true });
  try {
    await host.open();
    await viewer.open();
    const states = [];
    viewer.on('state', (id, s) => states.push(s));
    const offer = await host.addPeer('v1', { session, secret, port: 0 });
    const answer = await viewer.addPeer('host', { session, secret, port: 0 });
    viewer.connectPeer('host', offer);
    host.connectPeer('v1', answer);
    await until(() => states.some((s) => s.state === 'connected'), 8000);
    assert.equal(states.at(-1).path, 'tcp');
    const got = await stream(host, viewer, { frames: 15, size: 4 * MAX_DATA });
    await until(() => got.length === 15);
    assert.ok(got.every((g) => g.ok));
    // a wrong secret is not let in
    const bad = new WebSocket(tcpUrl);
    const closed = await new Promise((resolve) => {
      bad.onopen = () => bad.send(JSON.stringify({ session, role: 2, nonce: 'aa', mac: Buffer.alloc(16).toString('base64') }));
      bad.onclose = (e) => resolve(e.code);
    });
    assert.equal(closed, 1008);
  } finally {
    host.close();
    viewer.close();
    await app.close();
  }
});

test('WebSocket path: the server does not queue more than ~150 ms towards a viewer that is too slow', async () => {
  const { WebSocket: WsClient } = await import('ws');
  const { TCP_MAX_WAIT_MS } = await import('../server/direct-relay.js');
  const { createApp } = await import('../server/server.js');
  const app = createApp({ port: 0, host: '127.0.0.1', hostPassword: 'x', turnSecret: '', directPort: 0 });
  await new Promise((r) => app.server.listen(0, '127.0.0.1', r));
  const url = `ws://127.0.0.1:${app.server.address().port}/direct`;
  const { session, secret } = app.relay.create({ room: 'r', viewer: 'v1' });
  const join = (role) => new Promise((resolve, reject) => {
    const ws = new WsClient(url);
    ws.on('open', () => {
      const nonce = crypto.randomBytes(8).toString('hex');
      const mac = crypto.createHmac('sha256', Buffer.from(secret, 'base64')).update(`tcp:${session}:${role}:${nonce}`).digest().subarray(0, 16);
      ws.send(JSON.stringify({ session, role, nonce, mac: mac.toString('base64') }));
    });
    ws.once('message', (m) => (String(m) === 'ok' ? resolve(ws) : reject(new Error(String(m)))));
    ws.on('error', reject);
  });
  const host = await join(1);
  const viewer = await join(2);
  try {
    const towardsViewer = [...app.relay.sessions.values()][0].tcp[2];
    viewer._socket.pause(); // a viewer whose line does not keep up
    const packet = Buffer.alloc(64 * 1024, 7);
    // the system's socket buffers fill first; then packets wait in the server's own queue
    for (let i = 0; i < 1000 && towardsViewer.bufferedAmount === 0; i++) {
      host.send(packet);
      await sleep(i % 20 === 19 ? 5 : 0);
    }
    assert.ok(towardsViewer.bufferedAmount > 0, 'the queue did not fill');
    // 30 ms apart for 1.5 s: without a limit in time 2 MB would wait (~0.8 s at 20 Mbit/s)
    let most = 0;
    for (let i = 0; i < 50; i++) {
      host.send(packet);
      await sleep(30);
      most = Math.max(most, towardsViewer.bufferedAmount);
    }
    const perPacket = packet.length + 10;
    assert.ok(most <= perPacket * (Math.ceil(TCP_MAX_WAIT_MS / 30) + 3), `queued ${most} bytes`);
  } finally {
    viewer._socket.resume();
    host.terminate();
    viewer.terminate();
    await app.close();
  }
});

/* ---- the browser's transport (public/direct-web.js) against the app's ---- */

async function loadWebDirect() {
  const fs = await import('node:fs');
  const vm = await import('node:vm');
  globalThis.window ||= globalThis; // what the script hangs its API on
  vm.runInThisContext(fs.readFileSync(new URL('../public/direct-web.js', import.meta.url), 'utf8'));
  return globalThis.ScreenShareWebDirect;
}

async function webPair(webRole) {
  const { createApp } = await import('../server/server.js');
  const app = createApp({ port: 0, host: '127.0.0.1', hostPassword: 'x', turnSecret: '', directPort: 0 });
  await new Promise((r) => app.server.listen(0, '127.0.0.1', r));
  const tcpUrl = `ws://127.0.0.1:${app.server.address().port}/direct`;
  const { session, secret } = app.relay.create({ room: 'r', viewer: 'v1' });
  const web = (await loadWebDirect()).create({ tcpUrl });
  const events = [];
  const media = [];
  web.onEvent((name, ...args) => events.push([name, ...args]));
  web.onMedia((meta, data) => media.push({ meta, data: Buffer.from(data) }));
  // the app on the other side, with its UDP socket (no path there: the browser has none)
  const native = new DirectEndpoint({ role: webRole === 'viewer' ? 'host' : 'viewer', serverHost: '127.0.0.1', tcpUrl });
  await native.open();
  const nativeStates = [];
  native.on('state', (id, s) => nativeStates.push(s));
  const info = { session, secret, port: 0 };
  if (webRole === 'viewer') {
    const offer = await native.addPeer('v1', info);
    const answer = await web.add('viewer', 'host', info);
    assert.equal(answer.tcp, true);
    web.connect('host', offer);
    native.connectPeer('v1', answer);
  } else {
    const offer = await web.add('host', 'v1', info);
    const answer = await native.addPeer('host', info);
    native.connectPeer('host', offer);
    web.connect('v1', answer);
  }
  await until(() => nativeStates.some((s) => s.state === 'connected') && events.some(([n, , s]) => n === 'state' && s.state === 'connected'), 8000);
  return {
    web, native, events, media, nativeStates,
    async close() {
      web.stop();
      native.close();
      await app.close();
    },
  };
}

test('browser transport: an app shares to a browser over the server WebSocket (whole frames, keyframes, clock)', async () => {
  const p = await webPair('viewer');
  try {
    assert.equal(p.nativeStates.at(-1).path, 'tcp'); // at once: the browser said it has only that
    for (let n = 0; n < 15; n++) {
      p.native.sendMedia({ stream: 0, key: n === 0, ts: n, capture: n }, frame(n, 6 * MAX_DATA + n));
      await sleep(5);
    }
    p.native.sendMedia({ stream: 1, sub: 0x21, ts: 1, capture: 1 }, Buffer.from('opus'));
    await until(() => p.media.filter((m) => m.meta.stream === 0).length === 15 && p.media.some((m) => m.meta.stream === 1));
    const video = p.media.filter((m) => m.meta.stream === 0);
    assert.deepEqual(video.map((m) => m.data.readUInt32BE(0)), [...Array(15).keys()]);
    assert.ok(video.every((m) => m.data.equals(frame(m.data.readUInt32BE(0), m.data.length))));
    assert.equal(p.media.find((m) => m.meta.stream === 1).meta.sub, 0x21);
    // the browser asks for a keyframe; it measures the round trip; its reports reach the app
    const keys = [];
    p.native.on('keyframe', (id) => keys.push(id));
    const reports = [];
    p.native.on('report', (id, r) => reports.push(r));
    p.web.keyframe();
    await until(() => keys.length === 1 && reports.length > 0 && p.events.some(([n]) => n === 'clock'), 3000);
    assert.equal(reports.at(-1).path, 'tcp');
    // a socket that cannot keep up: frames are dropped (not queued) until the next keyframe
    const ws = p.native.peers.get('v1').tcp;
    Object.defineProperty(ws, 'bufferedAmount', { value: 10 << 20, configurable: true });
    const before = p.media.length;
    p.native.sendMedia({ stream: 0, key: false, ts: 20, capture: 20 }, frame(20, 1000));
    assert.equal(keys.length, 2, 'the sharer is told to send a keyframe');
    delete ws.bufferedAmount;
    p.native.sendMedia({ stream: 0, key: false, ts: 21, capture: 21 }, frame(21, 1000));
    p.native.sendMedia({ stream: 0, key: true, ts: 22, capture: 22 }, frame(22, 1000));
    await until(() => p.media.length > before);
    await sleep(100);
    assert.deepEqual(p.media.slice(before).map((m) => m.data.readUInt32BE(0)), [22]);
  } finally {
    await p.close();
  }
});

test('browser transport: a browser shares to an app', async () => {
  const p = await webPair('host');
  const got = [];
  p.native.on('media', (meta, data) => { if (meta.stream === 0) got.push(data); });
  try {
    for (let n = 0; n < 10; n++) {
      p.web.send({ stream: 0, key: n === 0, ts: n, capture: n }, new Uint8Array(frame(n, 50000)));
      await sleep(5);
    }
    await until(() => got.length === 10);
    assert.deepEqual(got.map((d) => d.readUInt32BE(0)), [...Array(10).keys()]);
    assert.ok(got.every((d) => d.equals(frame(d.readUInt32BE(0), d.length))));
    // the app's keyframe request reaches the browser sharer
    p.native.keyframe('host');
    await until(() => p.events.some(([n]) => n === 'keyframe'), 3000);
  } finally {
    await p.close();
  }
});

test('browser transport: without WebCrypto X25519 the key exchange is our WebAssembly – the app still connects', async () => {
  const fs = await import('node:fs');
  const vm = await import('node:vm');
  vm.runInThisContext(fs.readFileSync(new URL('../public/ss-web.js', import.meta.url), 'utf8'));
  const instance = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(new URL('../public/ss-web.wasm', import.meta.url))), {});
  const ours = globalThis.ScreenShareWasm.wrap(instance.exports);
  let used = 0;
  const load = globalThis.ScreenShareWasm.load;
  globalThis.ScreenShareWasm.load = async () => {
    used++;
    return ours;
  };
  const subtle = globalThis.crypto.subtle;
  const generateKey = subtle.generateKey;
  subtle.generateKey = function (alg, ...rest) {
    if (alg?.name === 'X25519') return Promise.reject(new DOMException('not supported', 'NotSupportedError')); // an older browser
    return generateKey.call(this, alg, ...rest);
  };
  try {
    const p = await webPair('viewer');
    try {
      assert.ok(used > 0, 'the WebAssembly was used');
      p.native.sendMedia({ stream: 0, key: true, ts: 1, capture: 1 }, frame(1, 5000));
      await until(() => p.media.some((m) => m.meta.stream === 0));
      assert.ok(p.media[0].data.equals(frame(1, 5000)));
    } finally {
      await p.close();
    }
  } finally {
    subtle.generateKey = generateKey;
    globalThis.ScreenShareWasm.load = load;
  }
});

test('browser transport: frames not confirmed in time (a queue on the way) are dropped, not left to wait', async () => {
  // a browser sharing to a browser: the viewer confirms each frame
  const { createApp } = await import('../server/server.js');
  const app = createApp({ port: 0, host: '127.0.0.1', hostPassword: 'x', turnSecret: '', directPort: 0 });
  await new Promise((r) => app.server.listen(0, '127.0.0.1', r));
  const tcpUrl = `ws://127.0.0.1:${app.server.address().port}/direct`;
  const { session, secret } = app.relay.create({ room: 'r', viewer: 'v1' });
  const api = await loadWebDirect();
  const host = api.create({ tcpUrl });
  const viewer = api.create({ tcpUrl });
  const hostEvents = [];
  const viewerEvents = [];
  const got = [];
  host.onEvent((name, ...args) => hostEvents.push([name, ...args]));
  viewer.onEvent((name, ...args) => viewerEvents.push([name, ...args]));
  viewer.onMedia((meta) => { if (meta.stream === 0) got.push(meta.ts); });
  try {
    const info = { session, secret, port: 0 };
    const offer = await host.add('host', 'v1', info);
    const answer = await viewer.add('viewer', 'host', info);
    host.connect('v1', answer);
    viewer.connect('host', offer);
    const connected = (events) => events.some(([n, , s]) => n === 'state' && s?.state === 'connected');
    await until(() => connected(hostEvents) && connected(viewerEvents), 8000);
    for (let n = 0; n < 10; n++) {
      host.send({ stream: 0, key: n === 0, ts: n, capture: n }, new Uint8Array(frame(n, 20000)));
      await sleep(20);
    }
    await until(() => got.length === 10);
    await sleep(50);
    assert.ok(!hostEvents.some(([n]) => n === 'keyframe'), 'nothing dropped while the frames are confirmed');
    // the viewer's line stalls: no confirmations come, the next frame would only wait behind the last
    viewer.stop();
    host.send({ stream: 0, key: false, ts: 10, capture: 10 }, new Uint8Array(frame(10, 20000)));
    await sleep(300);
    host.send({ stream: 0, key: false, ts: 11, capture: 11 }, new Uint8Array(frame(11, 20000)));
    assert.ok(hostEvents.some(([n]) => n === 'keyframe'), 'the late frame is dropped and a keyframe asked for');
  } finally {
    host.stop();
    viewer.stop();
    await app.close();
  }
});
